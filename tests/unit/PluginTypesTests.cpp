// plugins/template/rmw.d.luau describes the rmw API for editors (luau-lsp). Luau's own analyzer
// loads it here and type-checks every script we ship in strict mode: a definition the API outgrew,
// or an example that misuses it, fails the build. The solver is luau-lsp's default; the new one (in
// Luau 0.729) still loses `any` through refinements (`not data` makes it `~(false?)`).

#include <Luau/BuiltinDefinitions.h>
#include <Luau/ConfigResolver.h>
#include <Luau/Error.h>
#include <Luau/FileResolver.h>
#include <Luau/Frontend.h>
#include <Luau/TypeArena.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace
{

std::filesystem::path root()
{
    return RMW_SOURCE_DIR;
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (text.starts_with("\xEF\xBB\xBF"))
    {
        text.erase(0, 3); // the host strips it too
    }
    return text;
}

struct Scripts : Luau::FileResolver
{
    std::map<std::string, std::string> sources;

    std::optional<Luau::SourceCode> readSource(const Luau::ModuleName& name) override
    {
        const auto found = sources.find(name);
        if (found == sources.end())
        {
            return std::nullopt;
        }
        return Luau::SourceCode{found->second, Luau::SourceCode::Module};
    }
};

// Luau's analyzer with Luau's globals and rmw.d.luau's, in strict mode.
class Analyzer
{
public:
    Analyzer()
        : m_frontend(Luau::SolverMode::Old, &m_scripts, &m_config)
    {
        m_config.defaultConfig.mode = Luau::Mode::Strict;
        Luau::registerBuiltinGlobals(m_frontend, m_frontend.globals);
        Luau::unfreeze(m_frontend.globals.globalTypes);
        const std::string definitions = readFile(root() / "plugins" / "template" / "rmw.d.luau");
        const auto loaded = m_frontend.loadDefinitionFile(m_frontend.globals, m_frontend.globals.globalScope,
                                                          definitions, "@rmw", /*captureComments=*/false);
        Luau::freeze(m_frontend.globals.globalTypes);
        for (const auto& error : loaded.parseResult.errors)
        {
            m_definitionErrors.push_back(
                std::format("rmw.d.luau:{}: {}", error.getLocation().begin.line + 1, error.getMessage()));
        }
        if (loaded.module)
        {
            for (const auto& error : loaded.module->errors)
            {
                m_definitionErrors.push_back(
                    std::format("rmw.d.luau:{}: {}", error.location.begin.line + 1, Luau::toString(error)));
            }
        }
        if (!loaded.success && m_definitionErrors.empty())
        {
            m_definitionErrors.emplace_back("rmw.d.luau did not load");
        }
    }

    [[nodiscard]] const std::vector<std::string>& definitionErrors() const noexcept
    {
        return m_definitionErrors;
    }

    // "<name>:<line>: <message>" for each error in `source`.
    std::vector<std::string> check(const std::string& name, std::string source)
    {
        m_scripts.sources[name] = std::move(source);
        m_frontend.markDirty(name); // the same name with new text: check it again
        std::vector<std::string> errors;
        for (const auto& error : m_frontend.check(name).errors)
        {
            errors.push_back(
                std::format("{}:{}: {}", name, error.location.begin.line + 1, Luau::toString(error)));
        }
        return errors;
    }

private:
    Scripts m_scripts;
    Luau::NullConfigResolver m_config;
    Luau::Frontend m_frontend;
    std::vector<std::string> m_definitionErrors;
};

// Every main.luau we ship: the examples and the template.
std::vector<std::filesystem::path> shippedScripts()
{
    std::vector<std::filesystem::path> scripts{root() / "plugins" / "template" / "main.luau"};
    for (const auto& entry : std::filesystem::directory_iterator(root() / "plugins" / "examples"))
    {
        if (std::filesystem::exists(entry.path() / "main.luau"))
        {
            scripts.push_back(entry.path() / "main.luau");
        }
    }
    return scripts;
}

TEST(PluginTypes, EveryShippedScriptChecksAgainstTheDefinitions)
{
    Analyzer analyzer;
    ASSERT_EQ(analyzer.definitionErrors(), std::vector<std::string>{});
    const auto shipped = shippedScripts();
    ASSERT_GE(shipped.size(), 7u);
    for (const auto& path : shipped)
    {
        const std::string name = std::filesystem::relative(path, root()).generic_string();
        EXPECT_EQ(analyzer.check(name, readFile(path)), std::vector<std::string>{});
    }
}

TEST(PluginTypes, TheDefinitionsCatchMistakes)
{
    Analyzer analyzer;
    ASSERT_EQ(analyzer.definitionErrors(), std::vector<std::string>{});
    ASSERT_EQ(analyzer.check("fine.luau", "--!strict\nrmw.action(\"bar.toggle\")"),
              std::vector<std::string>{});
    int number = 0;
    for (const std::string_view mistake : {
             R"(rmw.action("bar.toggel"))",                   // no such action
             R"(rmw.on("system.cpus", function(value) end))", // no such provider
             R"(rmw.every("1000", function() end))",          // a number of ms
             R"(rmw.http.get("https://a.example.com", 3))",   // no callback
             R"(local n: number = rmw.json.encode({}))",      // encode gives text
             R"(rmw.widget(function(w) w:set(1) end))",       // a table or a name
             R"(rmw.on("system.cpu", function(cpu: RmwCpu) local s: string = cpu.usage end))",
         })
    {
        const std::string name = std::format("mistake{}.luau", ++number);
        EXPECT_NE(analyzer.check(name, "--!strict\n" + std::string(mistake)), std::vector<std::string>{})
            << mistake;
    }
}

} // namespace
