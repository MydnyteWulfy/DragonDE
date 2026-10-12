// dragon-scanner: Wayland protocol XML -> C++ server-side bindings.
//
// Builds on top of wayland-scanner's C output: the generated code includes the
// C server header (implementation structs, *_send_* helpers) and links against
// the C private-code (wl_interface tables). This tool adds typed C++ classes
// on top, deriving from dragon::Resource.
//
// Usage: dragon-scanner <protocol.xml> <out.h> <out.cpp> --c-header <name>
//                       [--skip iface1,iface2,...]

#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

// ---------------------------------------------------------------- model --

struct Arg {
    std::string name;
    std::string type; // int uint fixed string object new_id array fd
    std::string interface;
    std::string enumRef;
    std::string summary;
    bool allowNull = false;
};

struct Message {
    std::string name;
    std::string summary;
    bool destructor = false;
    std::uint32_t since = 1;
    std::vector<Arg> args;
};

struct Entry {
    std::string name;
    std::string value;
    std::string summary;
};

struct Enum {
    std::string name;
    std::string summary;
    bool bitfield = false;
    std::vector<Entry> entries;
};

struct Interface {
    std::string name;
    std::string summary;
    std::uint32_t version = 1;
    std::vector<Message> requests;
    std::vector<Message> events;
    std::vector<Enum> enums;
};

struct Protocol {
    std::string name;
    std::string sourceFile;
    std::vector<Interface> interfaces;
};

// -------------------------------------------------------------- helpers --

const std::set<std::string_view> kKeywords = {
    "alignas", "alignof", "and", "asm", "auto", "bool", "break", "case", "catch", "char",
    "class", "concept", "const", "consteval", "constexpr", "constinit", "continue", "default",
    "delete", "do", "double", "else", "enum", "explicit", "export", "extern", "false", "float",
    "for", "friend", "goto", "if", "inline", "int", "interface", "long", "mutable", "namespace",
    "new", "noexcept", "not", "nullptr", "operator", "or", "private", "protected", "public",
    "register", "requires", "return", "short", "signed", "sizeof", "static", "struct", "switch",
    "template", "this", "throw", "true", "try", "typedef", "typename", "union", "unsigned",
    "using", "virtual", "void", "volatile", "while", "xor",
    // names our generated code uses as parameters/members
    "resource", "client", "version", "id", "self",
};

std::string pascal(std::string_view snake)
{
    std::string out;
    bool upper = true;
    for (char c : snake) {
        if (c == '_') {
            upper = true;
            continue;
        }
        out += upper ? char(std::toupper(static_cast<unsigned char>(c))) : c;
        upper = false;
    }
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0])))
        out.insert(out.begin(), '_');
    return out;
}

std::string camel(std::string_view snake)
{
    auto s = pascal(snake);
    if (s[0] != '_')
        s[0] = char(std::tolower(static_cast<unsigned char>(s[0])));
    return s;
}

std::string ident(std::string_view name)
{
    std::string s(name);
    if (kKeywords.contains(s))
        s += '_';
    return s;
}

// One-line comment text: collapse whitespace.
std::string oneLine(std::string_view text)
{
    std::string out;
    bool space = false;
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            space = !out.empty();
            continue;
        }
        if (space)
            out += ' ';
        out += c;
        space = false;
    }
    return out;
}

std::string summaryOf(const pugi::xml_node& node)
{
    if (auto d = node.child("description"))
        return oneLine(d.attribute("summary").as_string());
    return oneLine(node.attribute("summary").as_string());
}

struct Param {
    std::string type;
    std::string name;
};

std::string joinParams(const std::vector<Param>& params, bool withNames)
{
    std::string out;
    for (const auto& p : params) {
        if (!out.empty())
            out += ", ";
        out += withNames ? p.type + " " + p.name : p.type;
    }
    return out;
}

// ---------------------------------------------------------------- parse --

std::optional<Protocol> parse(const std::filesystem::path& path)
{
    pugi::xml_document doc;
    auto result = doc.load_file(path.c_str());
    if (!result) {
        std::fprintf(stderr, "dragon-scanner: %s: %s\n", path.c_str(), result.description());
        return std::nullopt;
    }

    auto root = doc.child("protocol");
    Protocol proto{root.attribute("name").as_string(), path.filename().string(), {}};

    auto parseMessage = [](const pugi::xml_node& node) {
        Message m;
        m.name = node.attribute("name").as_string();
        m.summary = summaryOf(node);
        m.destructor = std::string_view(node.attribute("type").as_string()) == "destructor";
        m.since = node.attribute("since").as_uint(1);
        for (auto a : node.children("arg")) {
            m.args.push_back({a.attribute("name").as_string(), a.attribute("type").as_string(),
                              a.attribute("interface").as_string(), a.attribute("enum").as_string(),
                              oneLine(a.attribute("summary").as_string()),
                              a.attribute("allow-null").as_bool(false)});
        }
        return m;
    };

    for (auto i : root.children("interface")) {
        Interface iface;
        iface.name = i.attribute("name").as_string();
        iface.version = i.attribute("version").as_uint(1);
        iface.summary = summaryOf(i);
        for (auto r : i.children("request"))
            iface.requests.push_back(parseMessage(r));
        for (auto e : i.children("event"))
            iface.events.push_back(parseMessage(e));
        for (auto e : i.children("enum")) {
            Enum en{e.attribute("name").as_string(), summaryOf(e), e.attribute("bitfield").as_bool(false), {}};
            for (auto entry : e.children("entry"))
                en.entries.push_back({entry.attribute("name").as_string(), entry.attribute("value").as_string(),
                                      summaryOf(entry)});
            iface.enums.push_back(std::move(en));
        }
        proto.interfaces.push_back(std::move(iface));
    }
    return proto;
}

// ------------------------------------------------------------- generate --

class Generator {
public:
    explicit Generator(const Protocol& proto) : m_proto(proto) {}

    std::string header(const std::string& cHeader) const;
    std::string source(const std::string& headerName) const;

private:
    static std::string className(std::string_view iface) { return pascal(iface); }
    static std::string enumTypeName(std::string_view iface, std::string_view en)
    {
        return pascal(iface) + pascal(en);
    }

    // Resolves an `enum="x"` / `enum="iface.x"` reference to our enum type, if
    // it's defined in this protocol. Cross-protocol enums fall back to uint32_t.
    std::optional<std::string> resolveEnum(const Interface& owner, const std::string& ref) const;

    std::vector<Param> requestParams(const Interface& iface, const Message& m) const;
    std::string eventParams(const Interface& iface, const Message& m) const;
    std::string cDispatchParams(const Message& m) const;
    std::string handlerCallArgs(const Interface& iface, const Message& m) const;
    std::string sendCallArgs(const Interface& iface, const Message& m) const;

    const Protocol& m_proto;
};

std::optional<std::string> Generator::resolveEnum(const Interface& owner, const std::string& ref) const
{
    if (ref.empty())
        return std::nullopt;
    std::string ifaceName = owner.name;
    std::string enumName = ref;
    if (auto dot = ref.find('.'); dot != std::string::npos) {
        ifaceName = ref.substr(0, dot);
        enumName = ref.substr(dot + 1);
    }
    for (const auto& i : m_proto.interfaces) {
        if (i.name != ifaceName)
            continue;
        for (const auto& e : i.enums) {
            if (e.name == enumName)
                return enumTypeName(i.name, e.name);
        }
    }
    return std::nullopt;
}

std::vector<Param> Generator::requestParams(const Interface& iface, const Message& m) const
{
    std::vector<Param> params;
    for (const auto& a : m.args) {
        const auto n = ident(camel(a.name));
        if (a.type == "int") params.push_back({resolveEnum(iface, a.enumRef).value_or("std::int32_t"), n});
        else if (a.type == "uint") params.push_back({resolveEnum(iface, a.enumRef).value_or("std::uint32_t"), n});
        else if (a.type == "fixed") params.push_back({"double", n});
        else if (a.type == "string") params.push_back({a.allowNull ? "const char*" : "std::string_view", n});
        else if (a.type == "object") params.push_back({"wl_resource*", n});
        else if (a.type == "array") params.push_back({"wl_array*", n});
        else if (a.type == "fd") params.push_back({"std::int32_t", n});
        else if (a.type == "new_id") {
            if (a.interface.empty()) {
                params.push_back({"std::string_view", n + "Interface"});
                params.push_back({"std::uint32_t", n + "Version"});
            }
            params.push_back({"std::uint32_t", n});
        }
    }
    return params;
}

std::string Generator::eventParams(const Interface& iface, const Message& m) const
{
    std::string out;
    for (const auto& a : m.args) {
        const auto n = ident(camel(a.name));
        std::string t;
        if (a.type == "int") t = resolveEnum(iface, a.enumRef).value_or("std::int32_t");
        else if (a.type == "fd") t = "std::int32_t";
        else if (a.type == "uint") t = resolveEnum(iface, a.enumRef).value_or("std::uint32_t");
        else if (a.type == "fixed") t = "double";
        else if (a.type == "string") t = "const char*";
        else if (a.type == "object" || a.type == "new_id") t = "wl_resource*";
        else if (a.type == "array") t = "wl_array*";
        out += (out.empty() ? "" : ", ") + t + " " + n;
    }
    return out;
}

std::string Generator::cDispatchParams(const Message& m) const
{
    std::string out = "wl_client*, wl_resource* resource";
    for (const auto& a : m.args) {
        const auto n = ident(camel(a.name));
        if (a.type == "int" || a.type == "fd") out += ", std::int32_t " + n;
        else if (a.type == "uint") out += ", std::uint32_t " + n;
        else if (a.type == "fixed") out += ", wl_fixed_t " + n;
        else if (a.type == "string") out += ", const char* " + n;
        else if (a.type == "object") out += ", wl_resource* " + n;
        else if (a.type == "array") out += ", wl_array* " + n;
        else if (a.type == "new_id") {
            if (a.interface.empty())
                out += ", const char* " + n + "Interface, std::uint32_t " + n + "Version";
            out += ", std::uint32_t " + n;
        }
    }
    return out;
}

std::string Generator::handlerCallArgs(const Interface& iface, const Message& m) const
{
    std::string out;
    auto add = [&](const std::string& s) { out += (out.empty() ? "" : ", ") + s; };
    for (const auto& a : m.args) {
        const auto n = ident(camel(a.name));
        if (a.type == "fixed") add("wl_fixed_to_double(" + n + ")");
        else if (a.type == "string" && !a.allowNull) add("std::string_view(" + n + ")");
        else if (a.type == "uint" || a.type == "int") {
            auto en = resolveEnum(iface, a.enumRef);
            add(en ? "static_cast<" + *en + ">(" + n + ")" : n);
        } else if (a.type == "new_id" && a.interface.empty()) {
            add("std::string_view(" + n + "Interface)");
            add(n + "Version");
            add(n);
        } else add(n);
    }
    return out;
}

std::string Generator::sendCallArgs(const Interface& iface, const Message& m) const
{
    std::string out = "resource()";
    for (const auto& a : m.args) {
        const auto n = ident(camel(a.name));
        if (a.type == "fixed") out += ", wl_fixed_from_double(" + n + ")";
        else if (a.type == "uint" && resolveEnum(iface, a.enumRef)) out += ", static_cast<std::uint32_t>(" + n + ")";
        else if (a.type == "int" && resolveEnum(iface, a.enumRef)) out += ", static_cast<std::int32_t>(" + n + ")";
        else out += ", " + n;
    }
    return out;
}

std::string Generator::header(const std::string& cHeader) const
{
    std::ostringstream o;
    o << "// Generated by dragon-scanner from " << m_proto.sourceFile << ". Do not edit.\n"
      << "#pragma once\n\n"
      << "#include \"wayland/Bitfield.h\"\n"
      << "#include \"wayland/Resource.h\"\n\n"
      << "#include <cstdint>\n"
      << "#include <string_view>\n"
      << "#include <" << cHeader << ">\n\n"
      << "namespace dragon::proto {\n";

    // Enums first, at namespace scope, so any class can use any enum regardless of order.
    for (const auto& iface : m_proto.interfaces) {
        for (const auto& en : iface.enums) {
            const auto type = enumTypeName(iface.name, en.name);
            o << "\n";
            if (!en.summary.empty())
                o << "/// " << iface.name << "." << en.name << ": " << en.summary << "\n";
            o << "enum class " << type << " : std::uint32_t {\n";
            for (const auto& e : en.entries) {
                o << "    " << pascal(e.name) << " = " << e.value << ",";
                if (!e.summary.empty())
                    o << " ///< " << e.summary;
                o << "\n";
            }
            o << "};\n";
            if (en.bitfield)
                o << "template <>\nstruct IsBitfield<" << type << "> : std::true_type {};\n";
        }
    }

    for (const auto& iface : m_proto.interfaces) {
        const auto cls = className(iface.name);
        o << "\n";
        if (!iface.summary.empty())
            o << "/// " << iface.name << ": " << iface.summary << "\n";
        o << "class " << cls << " : public Resource {\n"
          << "public:\n"
          << "    static const wl_interface* interface() { return &" << iface.name << "_interface; }\n"
          << "    static constexpr std::uint32_t maxVersion = " << iface.version << ";\n";

        if (!iface.enums.empty()) {
            o << "\n";
            for (const auto& en : iface.enums)
                o << "    using " << pascal(en.name) << " = " << enumTypeName(iface.name, en.name) << ";\n";
        }

        o << "\n    " << cls << "(wl_client* client, std::uint32_t version, std::uint32_t id);\n\n"
          << "    // nullptr if `resource` is null or isn't a " << cls << ".\n"
          << "    static " << cls << "* fromResource(wl_resource* resource);\n";

        if (!iface.events.empty()) {
            o << "\n    // Events. Ones newer than the bound version() are silently dropped.\n";
            for (const auto& ev : iface.events) {
                if (!ev.summary.empty())
                    o << "    /// " << ev.summary << (ev.since > 1 ? std::format(" (since v{})", ev.since) : "") << "\n";
                o << "    void send" << pascal(ev.name) << "(" << eventParams(iface, ev) << ");\n";
            }
        }

        if (!iface.requests.empty()) {
            o << "\nprotected:\n"
              << "    // Requests. Defaults log a warning; destructor requests destroy the\n"
              << "    // resource after the handler returns (don't call destroy() in them).\n";
            for (const auto& rq : iface.requests) {
                if (!rq.summary.empty())
                    o << "    /// " << rq.summary << (rq.destructor ? " [destructor]" : "") << "\n";
                o << "    virtual void handle" << pascal(rq.name) << "(" << joinParams(requestParams(iface, rq), true) << ");\n";
            }

            o << "\nprivate:\n"
              << "    static const struct " << iface.name << "_interface s_implementation;\n";
            for (const auto& rq : iface.requests)
                o << "    static void dispatch" << pascal(rq.name) << "(" << cDispatchParams(rq) << ");\n";
        }
        o << "};\n";
    }

    o << "\n} // namespace dragon::proto\n";
    return o.str();
}

std::string Generator::source(const std::string& headerName) const
{
    std::ostringstream o;
    o << "// Generated by dragon-scanner from " << m_proto.sourceFile << ". Do not edit.\n"
      << "#include \"" << headerName << "\"\n\n"
      << "#include \"core/Log.h\"\n\n"
      << "#include <wayland-server-core.h>\n\n"
      << "namespace dragon::proto {\n";

    for (const auto& iface : m_proto.interfaces) {
        const auto cls = className(iface.name);
        const bool hasRequests = !iface.requests.empty();
        const std::string impl = hasRequests ? "&s_implementation" : "nullptr";

        o << "\n// ---- " << iface.name << " ----\n\n";

        if (hasRequests) {
            o << "const struct " << iface.name << "_interface " << cls << "::s_implementation = {\n";
            for (const auto& rq : iface.requests)
                o << "    dispatch" << pascal(rq.name) << ",\n";
            o << "};\n\n";
        }

        o << cls << "::" << cls << "(wl_client* client, std::uint32_t version, std::uint32_t id)\n"
          << "    : Resource(client, interface(), version, id, " << impl << ")\n{\n}\n\n";

        o << cls << "* " << cls << "::fromResource(wl_resource* resource)\n{\n"
          << "    return static_cast<" << cls << "*>(fromResourceChecked(resource, interface(), " << impl
          << "));\n}\n";

        for (const auto& ev : iface.events) {
            o << "\nvoid " << cls << "::send" << pascal(ev.name) << "(" << eventParams(iface, ev) << ")\n{\n"
              << "    if (!resource()";
            if (ev.since > 1)
                o << " || version() < " << ev.since;
            o << ")\n        return;\n"
              << "    " << iface.name << "_send_" << ev.name << "(" << sendCallArgs(iface, ev) << ");\n}\n";
        }

        for (const auto& rq : iface.requests) {
            // Default handler: parameters unnamed to avoid unused warnings.
            const auto unnamed = joinParams(requestParams(iface, rq), false);
            o << "\nvoid " << cls << "::handle" << pascal(rq.name) << "(" << unnamed << ")\n{\n";
            if (!rq.destructor)
                o << "    log::warn(\"proto\", \"unhandled request " << iface.name << "." << rq.name << "\");\n";
            o << "}\n";
        }

        for (const auto& rq : iface.requests) {
            o << "\nvoid " << cls << "::dispatch" << pascal(rq.name) << "(" << cDispatchParams(rq) << ")\n{\n"
              << "    if (auto* self = static_cast<" << cls
              << "*>(static_cast<Resource*>(wl_resource_get_user_data(resource))))\n"
              << "        self->handle" << pascal(rq.name) << "(" << handlerCallArgs(iface, rq) << ");\n";
            if (rq.destructor)
                o << "    wl_resource_destroy(resource);\n";
            o << "}\n";
        }
    }

    o << "\n} // namespace dragon::proto\n";
    return o.str();
}

bool writeFile(const std::filesystem::path& path, const std::string& content)
{
    // Only touch the file if it changed, so dependents don't rebuild needlessly.
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream existing;
        existing << in.rdbuf();
        if (in && existing.str() == content)
            return true;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    if (!out) {
        std::fprintf(stderr, "dragon-scanner: cannot write %s\n", path.c_str());
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string_view> positional;
    std::string cHeader;
    std::set<std::string> skip;

    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--c-header" && i + 1 < argc) {
            cHeader = argv[++i];
        } else if (a == "--skip" && i + 1 < argc) {
            std::string_view list = argv[++i];
            while (!list.empty()) {
                auto comma = list.find(',');
                skip.emplace(list.substr(0, comma));
                list = comma == std::string_view::npos ? "" : list.substr(comma + 1);
            }
        } else {
            positional.push_back(a);
        }
    }

    if (positional.size() != 3 || cHeader.empty()) {
        std::fprintf(stderr, "usage: dragon-scanner <protocol.xml> <out.h> <out.cpp> --c-header <name> "
                             "[--skip iface,...]\n");
        return 2;
    }

    auto proto = parse(std::filesystem::path(positional[0]));
    if (!proto)
        return 1;
    std::erase_if(proto->interfaces, [&](const Interface& i) { return skip.contains(i.name); });

    const std::filesystem::path outH(positional[1]);
    const std::filesystem::path outCpp(positional[2]);
    Generator gen(*proto);
    if (!writeFile(outH, gen.header(cHeader)) || !writeFile(outCpp, gen.source(outH.filename().string())))
        return 1;
    return 0;
}
