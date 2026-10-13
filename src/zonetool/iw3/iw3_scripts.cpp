#include "iw3_scripts.h"

#include "resources.h"

#include "common/log.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <array>
#include <cctype>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace iw3::scripts
{
namespace
{
#include "iw8_gsc_builtins.inc"

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool IsBuiltin(const std::string &name)
{
    return std::binary_search(kIw8GscFunctions.begin(), kIw8GscFunctions.end(), name) ||
           std::binary_search(kIw8GscMethods.begin(), kIw8GscMethods.end(), name);
}

std::string CompatText()
{
    const HRSRC resource =
        FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_IW3_COMPAT_GSC), MAKEINTRESOURCEW(10));
    if (!resource)
        throw std::runtime_error("IW3 script compatibility layer resource is missing");
    const HGLOBAL loaded = LoadResource(nullptr, resource);
    const auto *data = static_cast<const char *>(LockResource(loaded));
    const DWORD size = SizeofResource(nullptr, resource);
    if (!loaded || !data || !size)
        throw std::runtime_error("IW3 script compatibility layer resource is empty");
    return std::string(data, data + size);
}

// IW3 builtins whose IW8 counterparts exist but must not run with IW3 data
// (sound aliases, renderer dvars, fog/vision/ambience owned by the converter
// or the Replay gametype), plus IW3-only builtins.
const std::set<std::string> &DroppedBuiltins()
{
    static const std::set<std::string> names{
        "ambientplay",        "ambientstop",       "setexpfog",          "setculldist",
        "visionsetnaked",     "visionsetnight",    "setdvar",            "setsaveddvar",
        "makedvarserverinfo", "setsunlight",       "resetsunlight",      "playsound",
        "playloopsound",      "stoploopsound",     "playsoundatpos",     "playsoundinspace",
        "playsoundasmaster",  "soundexists",       "precacheshader",     "precachestring",
        "precacheitem",       "precacherumble",    "precacheturret",     "precachevehicle",
        "precachemenu",       "precacheheadicon",  "precachestatusicon", "setmapcenter",
        "setteamradar",       "logstring",         "println",            "setclientnamemode",
    };
    return names;
}

// Functions IW3 maps call in its common scripts.
std::string CommonFunction(const std::string &file, const std::string &name)
{
    if (file == "maps/mp/_load" && name == "main")
        return "iw3compat_load_main";
    if (file == "maps/mp/_utility" && name == "exploder")
        return "iw3compat_exploder";
    return {};
}

enum class Kind
{
    Ident,
    String,
    Number,
    Punct,
    Anim,
};

struct Token
{
    Kind kind;
    std::string text;
};

struct Lexed
{
    std::vector<Token> tokens;
    std::vector<std::string> includes; // normalized paths, no extension
};

std::string NormalizePath(std::string path)
{
    path = Lower(path);
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.ends_with(".gsc"))
        path.resize(path.size() - 4);
    return path;
}

Lexed Lex(const std::string &s)
{
    Lexed out;
    static constexpr std::array<std::string_view, 24> punct2{
        "::", "[[", "]]", "==", "!=", "<=", ">=", "&&", "||", "++", "--", "+=",
        "-=", "*=", "/=", "%=", "|=", "&=", "^=", "<<", ">>", "->", "..", "@@"};
    std::size_t i = 0;
    const auto peek = [&](const std::size_t at) { return at < s.size() ? s[at] : '\0'; };
    while (i < s.size())
    {
        const char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c)))
        {
            ++i;
            continue;
        }
        if (c == '/' && peek(i + 1) == '/')
        {
            const auto end = s.find('\n', i);
            i = end == std::string::npos ? s.size() : end + 1;
            continue;
        }
        if (c == '/' && peek(i + 1) == '*')
        {
            const auto end = s.find("*/", i + 2);
            i = end == std::string::npos ? s.size() : end + 2;
            continue;
        }
        if (c == '/' && peek(i + 1) == '#')
        {
            // Developer blocks never run in a shipped game.
            const auto end = s.find("#/", i + 2);
            i = end == std::string::npos ? s.size() : end + 2;
            continue;
        }
        if (c == '#' && s.compare(i, 9, "#animtree") == 0)
        {
            // An animtree reference inside an expression; IW8 animtrees differ.
            out.tokens.push_back({Kind::Anim, "#animtree"});
            i += 9;
            continue;
        }
        if (c == '#')
        {
            const auto end = s.find_first_of(";\n", i);
            std::string directive = s.substr(i, (end == std::string::npos ? s.size() : end) - i);
            i = end == std::string::npos ? s.size() : end + 1;
            std::istringstream words(directive);
            std::string keyword, path;
            words >> keyword >> path;
            if (Lower(keyword) == "#include" && !path.empty())
                out.includes.push_back(NormalizePath(path));
            continue;
        }
        if (c == '"' || (c == '&' && peek(i + 1) == '"'))
        {
            std::size_t j = i + (c == '&' ? 2 : 1);
            std::string text = "\"";
            while (j < s.size() && s[j] != '"')
            {
                if (s[j] == '\\' && j + 1 < s.size())
                {
                    text += s[j];
                    ++j;
                }
                if (s[j] == '\n')
                    break;
                text += s[j];
                ++j;
            }
            text += '"';
            out.tokens.push_back({Kind::String, text});
            i = j < s.size() ? j + 1 : s.size();
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && std::isdigit(static_cast<unsigned char>(peek(i + 1)))))
        {
            std::size_t j = i;
            while (j < s.size() &&
                   (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '.' ||
                    ((s[j] == '-' || s[j] == '+') && (s[j - 1] == 'e' || s[j - 1] == 'E'))))
                ++j;
            out.tokens.push_back({Kind::Number, s.substr(i, j - i)});
            i = j;
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
        {
            std::size_t j = i;
            while (j < s.size() &&
                   (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_' || s[j] == '\\'))
                ++j;
            out.tokens.push_back({Kind::Ident, s.substr(i, j - i)});
            i = j;
            continue;
        }
        if (c == '%' && (std::isalpha(static_cast<unsigned char>(peek(i + 1))) || peek(i + 1) == '_'))
        {
            std::size_t j = i + 1;
            while (j < s.size() &&
                   (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_'))
                ++j;
            out.tokens.push_back({Kind::Anim, s.substr(i, j - i)});
            i = j;
            continue;
        }
        bool matched = false;
        for (const auto candidate : punct2)
            if (s.compare(i, candidate.size(), candidate) == 0)
            {
                out.tokens.push_back({Kind::Punct, std::string(candidate)});
                i += candidate.size();
                matched = true;
                break;
            }
        if (!matched)
        {
            out.tokens.push_back({Kind::Punct, std::string(1, c)});
            ++i;
        }
    }
    return out;
}

struct Function
{
    std::string name;
    std::vector<Token> params;
    std::vector<Token> body;
};

std::vector<Function> ParseFunctions(const std::vector<Token> &tokens, const std::string &file)
{
    std::vector<Function> functions;
    std::size_t i = 0;
    const auto fail = [&](const char *what) {
        throw std::runtime_error("IW3 script " + file + ": " + what);
    };
    while (i < tokens.size())
    {
        if (tokens[i].kind != Kind::Ident || i + 1 >= tokens.size() || tokens[i + 1].text != "(")
            fail("expected a function definition");
        Function function;
        function.name = Lower(tokens[i].text);
        i += 2;
        int depth = 1;
        while (i < tokens.size() && depth)
        {
            if (tokens[i].text == "(")
                ++depth;
            else if (tokens[i].text == ")" && --depth == 0)
                break;
            function.params.push_back(tokens[i++]);
        }
        if (i >= tokens.size())
            fail("unterminated parameter list");
        ++i;
        if (i >= tokens.size() || tokens[i].text != "{")
            fail("expected a function body");
        ++i;
        depth = 1;
        while (i < tokens.size())
        {
            if (tokens[i].kind == Kind::Punct && tokens[i].text == "{")
                ++depth;
            else if (tokens[i].kind == Kind::Punct && tokens[i].text == "}" && --depth == 0)
                break;
            function.body.push_back(tokens[i++]);
        }
        if (i >= tokens.size())
            fail("unterminated function body");
        ++i;
        functions.push_back(std::move(function));
    }
    return functions;
}

struct File
{
    std::string path; // normalized, no extension
    std::string prefix;
    Lexed lexed;
    std::vector<Function> functions;
    std::set<std::string> names;
};

const std::set<std::string> &Keywords()
{
    static const std::set<std::string> words{
        "if",        "else",          "while",       "for",     "foreach",   "in",
        "switch",    "case",          "default",     "return",  "break",     "continue",
        "wait",      "waittill",      "waittillmatch", "waittillframeend", "notify",
        "endon",     "thread",        "childthread", "isdefined", "istrue",  "true",
        "false",     "undefined",     "self",        "level",   "game",      "anim",
        "size",      "waitframe",     "do"};
    return words;
}

class Translator
{
  public:
    Translator(const std::string &map, const std::vector<Source> &sources,
               const std::function<std::string(const std::string &)> &effectFor)
        : map_(Lower(map)), effectFor_(effectFor)
    {
        for (const Source &source : sources)
            available_[NormalizePath(source.path)] = &source;
    }

    Translation Run(const std::string &generated)
    {
        Translation result;
        const std::string mainPath = "maps/mp/" + map_;
        std::deque<std::string> pending;
        if (available_.contains(mainPath))
            pending.push_back(mainPath);
        while (!pending.empty())
        {
            const std::string path = pending.front();
            pending.pop_front();
            if (files_.contains(path))
                continue;
            File file;
            file.path = path;
            const std::string stem = path.substr(path.rfind('/') + 1);
            file.prefix = "mw120r_" + stem + "_";
            file.lexed = Lex(available_.at(path)->text);
            file.functions = ParseFunctions(file.lexed.tokens, path);
            for (const auto &function : file.functions)
                file.names.insert(function.name);
            for (const auto &include : file.lexed.includes)
                if (available_.contains(include))
                    pending.push_back(include);
            for (std::size_t i = 0; i + 2 < file.lexed.tokens.size(); ++i)
                if (file.lexed.tokens[i].kind == Kind::Ident &&
                    file.lexed.tokens[i].text.find('\\') != std::string::npos &&
                    file.lexed.tokens[i + 1].text == "::")
                {
                    const auto referenced = NormalizePath(file.lexed.tokens[i].text);
                    if (available_.contains(referenced))
                        pending.push_back(referenced);
                }
            order_.push_back(path);
            files_.emplace(path, std::move(file));
        }

        std::ostringstream out;
        out << "// Converted from IW3 map scripts by iw8-zonetool for Replay 1.20.\n\n";
        bool hasMain = false;
        for (const auto &path : order_)
        {
            const File &file = files_.at(path);
            for (const Function &function : file.functions)
            {
                const bool isMain = path == mainPath && function.name == "main";
                hasMain = hasMain || isMain;
                out << (isMain ? std::string("main") : file.prefix + function.name) << "(";
                // IW3 accepted repeated parameter names (setAnim( a, val, val, val ));
                // IW8 does not, and only the first can be referenced anyway.
                std::set<std::string> seen;
                for (std::size_t p = 0; p < function.params.size(); ++p)
                {
                    std::string param = Lower(function.params[p].text);
                    if (function.params[p].kind == Kind::Ident && !seen.insert(param).second)
                        param += "_unused" + std::to_string(p);
                    out << (p ? " " : "") << param;
                }
                out << ")\n{\n";
                if (isMain)
                    out << "    iw3compat_minimap();\n    iw3compat_generated();\n";
                out << RenderBody(file, function.body, result) << "}\n\n";
            }
        }
        if (!hasMain)
            out << "main()\n{\n    iw3compat_minimap();\n    iw3compat_generated();\n"
                   "    iw3compat_load_main();\n}\n\n";
        out << CompatText() << "\n" << generated;
        result.gsc = out.str();
        result.translatedMain = hasMain;
        result.notes = std::move(notes_);
        return result;
    }

  private:
    std::string Resolve(const File &file, const std::string &qualified, const std::string &name)
    {
        if (!qualified.empty())
        {
            if (const auto local = files_.find(qualified); local != files_.end())
            {
                if (local->second.names.contains(name))
                    return local->second.prefix + name;
                Note("missing " + qualified + "::" + name);
                return "iw3compat_noop";
            }
            if (auto common = CommonFunction(qualified, name); !common.empty())
                return common;
            Note("IW3 common script call " + qualified + "::" + name + " dropped");
            return "iw3compat_noop";
        }
        if (file.names.contains(name))
            return file.prefix + name;
        for (const auto &include : file.lexed.includes)
        {
            if (const auto local = files_.find(include);
                local != files_.end() && local->second.names.contains(name))
                return local->second.prefix + name;
            if (auto common = CommonFunction(include, name); !common.empty())
                return common;
        }
        if (DroppedBuiltins().contains(name))
        {
            Note("IW3 builtin " + name + " dropped");
            return "iw3compat_noop";
        }
        if (IsBuiltin(name))
            return name;
        Note("unknown function " + name + " dropped");
        return "iw3compat_noop";
    }

    void Note(const std::string &text)
    {
        if (std::find(notes_.begin(), notes_.end(), text) == notes_.end())
            notes_.push_back(text);
    }

    static std::size_t SkipBalanced(const std::vector<Token> &tokens, std::size_t open)
    {
        // tokens[open] is "(": returns the index after its matching ")".
        int depth = 0;
        for (std::size_t i = open; i < tokens.size(); ++i)
        {
            if (tokens[i].kind != Kind::Punct)
                continue;
            if (tokens[i].text == "(")
                ++depth;
            else if (tokens[i].text == ")" && --depth == 0)
                return i + 1;
        }
        return tokens.size();
    }

    std::string RenderBody(const File &file, const std::vector<Token> &body, Translation &result)
    {
        std::vector<Token> out;
        const auto statementStart = [&] {
            if (out.empty())
                return true;
            const Token &last = out.back();
            return last.kind == Kind::Punct &&
                   (last.text == ";" || last.text == "{" || last.text == "}");
        };
        for (std::size_t i = 0; i < body.size();)
        {
            const Token &token = body[i];
            // IW3 team/faction setup belongs to the Replay gametype.
            if (token.kind == Kind::Ident && Lower(token.text) == "game" && i + 1 < body.size() &&
                body[i + 1].text == "[" && statementStart())
            {
                while (i < body.size() && !(body[i].kind == Kind::Punct && body[i].text == ";"))
                    ++i;
                ++i;
                Note("IW3 game[] faction assignment dropped");
                continue;
            }
            if (token.kind == Kind::Anim)
            {
                out.push_back({Kind::Ident, "undefined"});
                Note("IW3 animation reference dropped");
                ++i;
                continue;
            }
            if (token.kind == Kind::String)
            {
                result.strings.insert(token.text.substr(1, token.text.size() - 2));
                out.push_back(token);
                ++i;
                continue;
            }
            // path::name (call or function pointer)
            if (token.kind == Kind::Ident && token.text.find('\\') != std::string::npos &&
                i + 2 < body.size() && body[i + 1].text == "::" &&
                body[i + 2].kind == Kind::Ident)
            {
                const std::string resolved =
                    Resolve(file, NormalizePath(token.text), Lower(body[i + 2].text));
                const bool call = i + 3 < body.size() && body[i + 3].text == "(";
                if (!call)
                {
                    out.push_back({Kind::Punct, "::"});
                    out.push_back({Kind::Ident, resolved});
                    i += 3;
                    continue;
                }
                out.push_back({Kind::Ident, resolved});
                if (resolved == "iw3compat_noop")
                {
                    i = SkipBalanced(body, i + 3);
                    out.push_back({Kind::Punct, "("});
                    out.push_back({Kind::Punct, ")"});
                }
                else
                    i += 3;
                continue;
            }
            // ::name (local function pointer)
            if (token.kind == Kind::Punct && token.text == "::" && i + 1 < body.size() &&
                body[i + 1].kind == Kind::Ident)
            {
                out.push_back(token);
                out.push_back({Kind::Ident, Resolve(file, {}, Lower(body[i + 1].text))});
                i += 2;
                continue;
            }
            if (token.kind == Kind::Ident && i + 1 < body.size() && body[i + 1].text == "(" &&
                !Keywords().contains(Lower(token.text)))
            {
                const std::string name = Lower(token.text);
                if (name == "vectorscale")
                {
                    // IW8 removed vectorscale( v, s ); emit ( ( v ) * ( s ) ).
                    const std::size_t end = SkipBalanced(body, i + 1);
                    std::vector<Token> args(body.begin() + static_cast<std::ptrdiff_t>(i + 2),
                                            body.begin() + static_cast<std::ptrdiff_t>(end - 1));
                    int depth = 0;
                    std::size_t comma = args.size();
                    for (std::size_t a = 0; a < args.size(); ++a)
                    {
                        if (args[a].text == "(" || args[a].text == "[")
                            ++depth;
                        else if (args[a].text == ")" || args[a].text == "]")
                            --depth;
                        else if (args[a].text == "," && depth == 0)
                        {
                            comma = a;
                            break;
                        }
                    }
                    if (comma == args.size())
                        throw std::runtime_error("IW3 script " + file.path +
                                                 ": vectorscale needs two arguments");
                    std::vector<Token> rewritten{{Kind::Punct, "("}, {Kind::Punct, "("}};
                    rewritten.insert(rewritten.end(), args.begin(),
                                     args.begin() + static_cast<std::ptrdiff_t>(comma));
                    rewritten.push_back({Kind::Punct, ")"});
                    rewritten.push_back({Kind::Punct, "*"});
                    rewritten.push_back({Kind::Punct, "("});
                    rewritten.insert(rewritten.end(),
                                     args.begin() + static_cast<std::ptrdiff_t>(comma + 1),
                                     args.end());
                    rewritten.push_back({Kind::Punct, ")"});
                    rewritten.push_back({Kind::Punct, ")"});
                    // Re-scan the rewritten tokens so nested calls are translated.
                    std::vector<Token> rest(rewritten);
                    rest.insert(rest.end(), body.begin() + static_cast<std::ptrdiff_t>(end),
                                body.end());
                    std::vector<Token> head(out.begin(), out.end());
                    const std::string tail = RenderBody(file, rest, result);
                    std::ostringstream joined;
                    joined << Render(head) << tail;
                    return joined.str();
                }
                const std::string resolved = Resolve(file, {}, name);
                out.push_back({Kind::Ident, resolved});
                if (resolved == "iw3compat_noop")
                {
                    i = SkipBalanced(body, i + 1);
                    out.push_back({Kind::Punct, "("});
                    out.push_back({Kind::Punct, ")"});
                    continue;
                }
                if (resolved == "loadfx" && i + 3 < body.size() && body[i + 2].kind == Kind::String &&
                    body[i + 3].text == ")")
                {
                    const std::string source = body[i + 2].text.substr(1, body[i + 2].text.size() - 2);
                    std::string effect = effectFor_(source);
                    if (effect.empty())
                        effect = StockEffectFor(source);
                    out.push_back({Kind::Punct, "("});
                    out.push_back({Kind::String, "\"" + effect + "\""});
                    out.push_back({Kind::Punct, ")"});
                    Note("effect " + source + " -> " + effect);
                    i += 4;
                    continue;
                }
                ++i;
                continue;
            }
            if (token.kind == Kind::Ident)
            {
                // Variables and fields are case-insensitive in IW3 and IW8.
                out.push_back({Kind::Ident, Lower(token.text)});
                ++i;
                continue;
            }
            out.push_back(token);
            ++i;
        }
        return Render(out);
    }

    static std::string Render(const std::vector<Token> &tokens)
    {
        std::string text = "    ";
        for (std::size_t i = 0; i < tokens.size(); ++i)
        {
            const Token &token = tokens[i];
            text += token.text;
            if (token.kind == Kind::Punct && (token.text == ";" || token.text == "{" || token.text == "}"))
                text += "\n    ";
            else
                text += ' ';
        }
        while (!text.empty() && (text.back() == ' '))
            text.pop_back();
        return text + "\n";
    }

    std::string map_;
    std::function<std::string(const std::string &)> effectFor_;
    std::map<std::string, const Source *> available_;
    std::map<std::string, File> files_;
    std::vector<std::string> order_;
    std::vector<std::string> notes_;
};
} // namespace

std::string StockEffectFor(const std::string &iw3Effect)
{
    const std::string name = Lower(iw3Effect);
    if (name.find("glass") != std::string::npos)
        return name.find("car") != std::string::npos || name.find("veh") != std::string::npos
                   ? "vfx/iw8/veh/scriptables/vfx_veh_glass_windshield"
                   : "vfx/code/glass/glass_shatter_32x32";
    if (name.find("explo") != std::string::npos)
        return "vfx/iw8/veh/scriptables/vfx_veh_explosion_civ";
    if (name.find("fire") != std::string::npos || name.find("flame") != std::string::npos)
        return "vfx/iw8/veh/scriptables/vfx_veh_fire_linger_med";
    if (name.find("smoke") != std::string::npos || name.find("steam") != std::string::npos)
        return "vfx/iw8/veh/scriptables/vfx_smoke_light_ch";
    return "vfx/iw8_mp/equipment/flashbang/vfx_spark_crack_npop";
}

Translation Translate(const std::string &map, const std::vector<Source> &sources,
                      const std::function<std::string(const std::string &)> &effectFor,
                      const std::string &generated)
{
    Translator translator(map, sources, effectFor);
    return translator.Run(generated);
}

std::vector<std::uint8_t> Compile(const std::string &gsc, const std::filesystem::path &gscTool,
                                  const std::filesystem::path &workDirectory,
                                  const std::string &stem)
{
    std::filesystem::create_directories(workDirectory);
    const auto source = workDirectory / (stem + ".gsc");
    {
        std::ofstream file(source, std::ios::binary);
        file << gsc;
        if (!file)
            throw std::runtime_error("cannot write " + source.string());
    }
    const auto compiled = workDirectory / "compiled" / "iw8" / (stem + ".gscbin");
    std::error_code ignored;
    std::filesystem::remove(compiled, ignored);
    const auto log = workDirectory / (stem + ".gsc-tool.log");

    std::wstring command = L"\"" + gscTool.wstring() + L"\" -m comp -g iw8 -s pc \"" +
                           source.filename().wstring() + L"\"";
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE output = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE)
        throw std::runtime_error("cannot create " + log.string());
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output;
    startup.hStdError = output;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const std::wstring directory = workDirectory.wstring();
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup,
                                        &process);
    CloseHandle(output);
    if (!started)
        throw std::runtime_error("cannot run gsc-tool: " + gscTool.string());
    WaitForSingleObject(process.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    std::ifstream bin(compiled, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(bin)),
                                    std::istreambuf_iterator<char>());
    if (bytes.size() < 16 || std::memcmp(bytes.data(), "GSC\0", 4) != 0)
    {
        std::ifstream report(log);
        std::string text((std::istreambuf_iterator<char>(report)), std::istreambuf_iterator<char>());
        throw std::runtime_error("gsc-tool failed for " + source.string() + ": " + text);
    }
    return bytes;
}
} // namespace iw3::scripts
