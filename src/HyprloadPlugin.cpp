#include "HyprloadPlugin.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include "Hyprload.hpp"
#include "src/helpers/MiscFunctions.hpp"
#include "toml/toml.hpp"
#include "types.hpp"

namespace hyprload::plugin {
    std::tuple<int, std::string> executeCommand(const std::string& command) {
        std::string result = "";
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe) {
            return std::make_tuple(-1, "Failed to execute command");
        }

        char buffer[128];
        while (!feof(pipe)) {
            if (fgets(buffer, 128, pipe) != nullptr) {
                result += buffer;
            }
        }

        int exit = pclose(pipe);

        return std::make_tuple(exit, result);
    }

    hyprload::Result<HyprloadManifest, std::string> getHyprlandManifest(const std::filesystem::path& sourcePath) {
        std::filesystem::path manifestPath = sourcePath / "hyprload.toml";

        if (!std::filesystem::exists(manifestPath)) {
            return hyprload::Result<HyprloadManifest, std::string>::err("Source does not have a hyprload.toml manifest");
        }

        toml::table manifest;
        try {
            manifest = toml::parse(manifestPath.string());
        } catch (const std::exception& e) { return hyprload::Result<HyprloadManifest, std::string>::err("Failed to parse source manifest: " + std::string(e.what())); }

        return hyprload::Result<HyprloadManifest, std::string>::ok(HyprloadManifest(manifest));
    }

    hyprload::Result<PluginManifest, std::string> getPluginManifest(const std::filesystem::path& sourcePath, const std::string& name) {

        auto hyprloadManifest = getHyprlandManifest(sourcePath);

        if (hyprloadManifest.isErr()) {
            return hyprload::Result<PluginManifest, std::string>::err(hyprloadManifest.unwrapErr());
        }

        std::optional<PluginManifest> pluginManifest;

        for (const auto& plugin : hyprloadManifest.unwrap().getPlugins()) {
            if (plugin.getName() == name) {
                pluginManifest = plugin;
                break;
            }
        }

        if (!pluginManifest.has_value()) {
            return hyprload::Result<PluginManifest, std::string>::err("Plugin does not have a manifest for " + name);
        }

        return hyprload::Result<PluginManifest, std::string>::ok(std::move(pluginManifest.value()));
    }

    hyprload::Result<std::monostate, std::string> buildPlugin(const std::filesystem::path& sourcePath, const std::string& name) {
        std::optional<std::filesystem::path> hyprlandHeadersPath = hyprload::getHyprlandHeadersPath();

        auto pluginManifestResult = getPluginManifest(sourcePath, name);

        if (pluginManifestResult.isErr()) {
            return hyprload::Result<std::monostate, std::string>::err(pluginManifestResult.unwrapErr());
        }

        const auto& pluginManifest = pluginManifestResult.unwrap();

        if (!hyprlandHeadersPath.has_value()) {
            return hyprload::Result<std::monostate, std::string>::err("Could not find hyprland headers. Refer to https://github.com/Duckonaut/hyprload#Setup");
        }

        std::string buildSteps = "export HYPRLAND_HEADERS=" + hyprlandHeadersPath.value().string() + " && cd " + sourcePath.string() + " && ";

        for (const std::string& step : pluginManifest.getBuildSteps()) {
            buildSteps += step + " && ";
        }

        buildSteps += "cd -";

        auto [exit, output] = executeCommand(buildSteps);

        if (exit != 0) {
            return hyprload::Result<std::monostate, std::string>::err("Failed to build plugin: " + output);
        }

        return hyprload::Result<std::monostate, std::string>::ok(std::monostate());
    }

    PluginManifest::PluginManifest(std::string&& name, const toml::table& manifest) {
        m_sName = name;

        std::vector<std::string> authors = std::vector<std::string>();

        if (manifest.contains("authors") && manifest["authors"].is_array()) {
            manifest["authors"].as_array()->for_each([&authors](const auto& value) {
                if (!value.is_string()) {
                    throw std::runtime_error("Author must be a string");
                }
                authors.push_back(value.as_string()->get());
            });
        } else if (manifest.contains("author")) {
            authors.push_back(manifest["author"].as_string()->get());
        }

        m_sAuthors = authors;

        if (manifest.contains("version")) {
            m_sVersion = manifest["version"].as_string()->get();
        } else {
            m_sVersion = "0.0.0";
        }

        if (manifest.contains("description")) {
            m_sDescription = manifest["description"].as_string()->get();
        } else {
            m_sDescription = "No description provided";
        }

        if (manifest.contains("build") && manifest["build"].is_table()) {
            const toml::table* build = manifest["build"].as_table();

            if (build->contains("output") && build->get("output")->is_string()) {
                m_pBinaryOutputPath = build->get("output")->as_string()->get();
            } else {
                m_pBinaryOutputPath = name + ".so";
            }

            if (build->contains("steps") && build->get("steps")->is_array()) {
                build->get("steps")->as_array()->for_each([&buildSteps = m_sBuildSteps](const toml::node& value) {
                    if (!value.is_string()) {
                        throw std::runtime_error("Build step must be a string");
                    }
                    buildSteps.push_back(value.as_string()->get());
                });
            } else {
                throw std::runtime_error("Plugin must have build steps");
            }
        } else {
            throw std::runtime_error("Plugin must have a build table");
        }
    }

    const std::string& PluginManifest::getName() const {
        return m_sName;
    }

    const std::vector<std::string>& PluginManifest::getAuthors() const {
        return m_sAuthors;
    }

    const std::string& PluginManifest::getVersion() const {
        return m_sVersion;
    }

    const std::string& PluginManifest::getDescription() const {
        return m_sDescription;
    }

    const std::filesystem::path& PluginManifest::getBinaryOutputPath() const {
        return m_pBinaryOutputPath;
    }

    const std::vector<std::string>& PluginManifest::getBuildSteps() const {
        return m_sBuildSteps;
    }

    HyprloadManifest::HyprloadManifest(const toml::table& manifest) {
        m_vPlugins = std::vector<PluginManifest>();
        manifest.for_each([&plugins = m_vPlugins](const toml::key& key, const toml::node& value) {
            if (value.is_table()) {
                const toml::table& table = *value.as_table();
                plugins.emplace_back(std::string(key.str()), table);
            }
        });
    }

    const std::vector<PluginManifest>& HyprloadManifest::getPlugins() const {
        return m_vPlugins;
    }

    bool PluginSource::operator==(const PluginSource& other) const {
        if (typeid(*this) != typeid(other)) {
            return false;
        }

        return this->isEquivalent(other);
    }

    GitPluginSource::GitPluginSource(std::string&& url, std::string&& branch) {
        m_sBranch = branch;

        if (url.find("https://") == 0) {
            m_sUrl = url;
        } else if (url.find("git@") == 0) {
            m_sUrl = url;
        } else {
            m_sUrl = "https://github.com/" + url + ".git";
        }

        m_pSourcePath = hyprload::getPluginsPath() / "src" / m_sUrl.substr(m_sUrl.find_last_of('/') + 1);
    }

    hyprload::Result<std::monostate, std::string> GitPluginSource::installSource() {
        std::string command = "git clone " + m_sUrl + " " + m_pSourcePath.string() + " --branch " + m_sBranch + " --depth 1";

        if (std::system(command.c_str()) != 0) {
            return hyprload::Result<std::monostate, std::string>::err("Failed to clone plugin source");
        }

        return hyprload::Result<std::monostate, std::string>::ok(std::monostate());
    }

    bool GitPluginSource::isSourceAvailable() {
        return std::filesystem::exists(m_pSourcePath / ".git");
    }

    bool GitPluginSource::isUpToDate() {
        std::string command = "git -C " + m_pSourcePath.string() + " remote update";

        if (std::system(command.c_str()) != 0) {
            return false;
        }

        command = "git -C " + m_pSourcePath.string() + " status -uno";

        auto [exit, output] = executeCommand(command);

        return exit != 0 && output.find("ahead") == std::string::npos;
    }

    hyprload::Result<std::monostate, std::string> GitPluginSource::update(const std::string& name) {
        std::string command = "git -C " + m_pSourcePath.string() + " pull";

        if (std::system(command.c_str()) != 0) {
            return hyprload::Result<std::monostate, std::string>::err("Failed to update plugin source");
        }

        return this->install(name);
    }

    hyprload::Result<std::monostate, std::string> GitPluginSource::install(const std::string& name) {
        if (!this->isSourceAvailable()) {
            return this->installSource();
        }

        auto result = build(name);

        if (result.isErr()) {
            return result;
        }

        auto pluginManifestResult = getPluginManifest(m_pSourcePath, name);

        if (pluginManifestResult.isErr()) {
            return hyprload::Result<std::monostate, std::string>::err(pluginManifestResult.unwrapErr());
        }

        auto pluginManifest = pluginManifestResult.unwrap();

        std::filesystem::path outputBinary = m_pSourcePath / pluginManifest.getBinaryOutputPath();

        if (!std::filesystem::exists(outputBinary)) {
            return hyprload::Result<std::monostate, std::string>::err("Plugin binary does not exist");
        }

        std::filesystem::path targetPath = hyprload::getPluginBinariesPath() / outputBinary.filename();

        if (std::filesystem::exists(targetPath)) {
            std::filesystem::remove(targetPath);
        }

        std::filesystem::copy(outputBinary, targetPath);

        return hyprload::Result<std::monostate, std::string>::ok(std::monostate());
    }

    hyprload::Result<std::monostate, std::string> GitPluginSource::build(const std::string& name) {
        return buildPlugin(m_pSourcePath, name);
    }

    bool GitPluginSource::isEquivalent(const PluginSource& other) const {
        const auto& otherLocal = static_cast<const GitPluginSource&>(other);

        return m_sUrl == otherLocal.m_sUrl && m_sBranch == otherLocal.m_sBranch && m_pSourcePath == otherLocal.m_pSourcePath;
    }

    LocalPluginSource::LocalPluginSource(std::filesystem::path&& path) : m_pSourcePath(path) {}

    hyprload::Result<std::monostate, std::string> LocalPluginSource::installSource() {
        return hyprload::Result<std::monostate, std::string>::ok(std::monostate());
    }

    bool LocalPluginSource::isSourceAvailable() {
        return std::filesystem::exists(m_pSourcePath);
    }

    bool LocalPluginSource::isUpToDate() {
        return false; // Always update local plugins, since they are not versioned
                      // and we don't know if they have changed.
    }

    hyprload::Result<std::monostate, std::string> LocalPluginSource::update(const std::string& name) {
        return this->install(name);
    }

    hyprload::Result<std::monostate, std::string> LocalPluginSource::install(const std::string& name) {
        if (!this->isSourceAvailable()) {
            return hyprload::Result<std::monostate, std::string>::err("Source for " + name + " does not exist");
        }

        auto result = build(name);

        if (result.isErr()) {
            return result;
        }

        auto pluginManifestResult = getPluginManifest(m_pSourcePath, name);

        if (pluginManifestResult.isErr()) {
            return hyprload::Result<std::monostate, std::string>::err(pluginManifestResult.unwrapErr());
        }

        const auto& pluginManifest = pluginManifestResult.unwrap();

        std::filesystem::path outputBinary = m_pSourcePath / pluginManifest.getBinaryOutputPath();

        if (!std::filesystem::exists(outputBinary)) {
            return hyprload::Result<std::monostate, std::string>::err("Plugin binary does not exist");
        }

        std::filesystem::path targetPath = hyprload::getPluginBinariesPath() / outputBinary.filename();

        if (std::filesystem::exists(targetPath)) {
            std::filesystem::remove(targetPath);
        }

        std::filesystem::copy(outputBinary, targetPath);

        return hyprload::Result<std::monostate, std::string>::ok(std::monostate());
    }

    hyprload::Result<std::monostate, std::string> LocalPluginSource::build(const std::string& name) {
        return buildPlugin(m_pSourcePath, name);
    }

    bool LocalPluginSource::isEquivalent(const PluginSource& other) const {
        const auto& otherLocal = static_cast<const LocalPluginSource&>(other);

        return m_pSourcePath == otherLocal.m_pSourcePath;
    }

    PluginRequirement::PluginRequirement(const toml::table& plugin) {
        std::string source;

        if (plugin.contains("git") && plugin["git"].is_string()) {
            source = plugin["git"].as_string()->get();

            std::string branch = "main";

            if (plugin.contains("branch") && plugin["branch"].is_string()) {
                branch = plugin["branch"].as_string()->get();
            }

            m_pSource = std::make_unique<GitPluginSource>(std::string(source), std::move(branch));
        } else if (plugin.contains("local") && plugin["local"].is_string()) {
            source = plugin["local"].as_string()->get();
            m_pSource = std::make_unique<LocalPluginSource>(std::filesystem::path(source));
        } else {
            throw std::runtime_error("Plugin must have a source");
        }

        if (plugin.contains("name") && plugin["name"].is_string()) {
            m_sName = plugin["name"].as_string()->get();
        } else {
            m_sName = source.substr(source.find_last_of('/') + 1);
        }

        m_pBinaryPath = hyprload::getPluginsPath() / "bin" / (m_sName + ".so");
    }

    const std::string& PluginRequirement::getName() const {
        return m_sName;
    }

    const std::filesystem::path& PluginRequirement::getBinaryPath() const {
        return m_pBinaryPath;
    }

    const PluginSource& PluginRequirement::getSource() const {
        return *m_pSource;
    }
}