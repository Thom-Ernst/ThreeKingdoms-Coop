#pragma once
// Test-only UI protocol, independent of Windows/the game so the actual safety rules are testable.
#include <string>
#include <vector>
#include <unordered_set>
#include <cstdint>
#include <cstdlib>

namespace twui {
constexpr size_t MaxNodes = 2000, MaxReply = 60 * 1024, MaxCommand = 510;
using Widget = uintptr_t;
struct Node {
    std::string id, state, text;
    bool visible = false, disabled = true;
};
struct Request { std::string verb, path, value; int depth = 3; };
inline bool pathParts(const std::string& path, std::vector<std::string>& parts) {
    parts.clear();
    if (path.empty() || path[0] != '/' || path.size() > MaxCommand) return false;
    if (path == "/") return true;
    size_t start = 1;
    while (start < path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        auto s = path.substr(start, end - start);
        if (s.empty() || s == "." || s == ".." || s.find_first_of("*?\\") != std::string::npos) return false;
        for (unsigned char c : s) if (c < 32 || c == 127) return false;
        parts.push_back(s);
        if (parts.size() > 64 || end + 1 == path.size()) return false;
        start = end + 1;
    }
    return !parts.empty();
}
inline bool addressableId(const std::string& id) {
    std::vector<std::string> parts;
    return pathParts("/" + id, parts) && parts.size() == 1;
}
inline std::string childSegment(const std::string& id, size_t index) {
    return addressableId(id) ? id : "#" + std::to_string(index);
}
inline std::string escaped(const std::string& value) {
    std::string out;
    for (char c : value) {
        if (c == '\\') out += "\\\\";
        else if (c == '\t') out += "\\t";
        else if (c == '\r') out += "\\r";
        else if (c == '\n') out += "\\n";
        else if ((unsigned char)c < 32 || c == 127) {
            const char* hex = "0123456789ABCDEF";
            out += "\\x"; out += hex[(unsigned char)c >> 4]; out += hex[(unsigned char)c & 15];
        }
        else out += c;
    }
    return out;
}
inline std::string fail(const std::string& reason) { return "fail " + escaped(reason) + "\n"; }
// An unreadable record name must not erase an otherwise valid present-player id.
inline std::string playerRow(uint32_t id, const std::string& recordName, const std::string& capturedName) {
    const auto& name = recordName.empty() ? capturedName : recordName;
    return "player\t" + std::to_string(id) + (name.empty() ? "" : "\t" + escaped(name)) + "\n";
}
inline bool parse(const std::string& command, Request& r, std::string& error) {
    r = Request{};
    if (command.size() > MaxCommand || command.find_first_of("\r\n") != std::string::npos || command.find('\0') != std::string::npos) {
        error = "invalid-command"; return false;
    }
    // Tabs preserve spaces in component Ids and text. Interactive commands can use spaces instead.
    std::vector<std::string> args;
    bool tabs = command.find('\t') != std::string::npos;
    size_t start = 0;
    while (start < command.size()) {
        if (!tabs) { while (start < command.size() && command[start] == ' ') ++start; }
        auto end = command.find(tabs ? '\t' : ' ', start);
        if (end == std::string::npos) end = command.size();
        args.push_back(command.substr(start, end - start));
        start = end + 1;
    }
    if (args.size() < 2 || args[0] != "ui") { error = "usage-ui"; return false; }
    r.verb = args[1];
    if (r.verb == "arm" || r.verb == "disarm" || r.verb == "status" || r.verb == "players" || r.verb == "quit" || r.verb == "roots" || r.verb == "modal") {
        if (args.size() == 2) return true;
    } else if (r.verb == "key" && args.size() == 3) {
        if (args[2] != "esc") { error = "unsupported-key"; return false; }
        r.value = args[2]; return true;
    } else if (r.verb == "tree" || r.verb == "click" || r.verb == "find") {
        if (args.size() >= 3 && args.size() <= (r.verb == "tree" ? 4u : 3u)) {
            r.path = args[2];
            std::vector<std::string> parts;
            if (r.verb == "find") {
                if (r.path.empty()) { error = "invalid-name"; return false; }
            } else if (!pathParts(r.path, parts)) { error = "invalid-path"; return false; }
            if (args.size() == 4) {
                if (args[3].size() != 1 || args[3][0] < '0' || args[3][0] > '8') { error = "depth-must-be-0..8"; return false; }
                r.depth = args[3][0] - '0';
            }
            return true;
        }
    } else if (r.verb == "text" && args.size() >= 4) {
        r.path = args[2]; r.value = args[3];
        for (size_t i = 4; i < args.size(); ++i) r.value += " " + args[i];
        std::vector<std::string> parts;
        if (!pathParts(r.path, parts)) { error = "invalid-path"; return false; }
        return true;
    }
    error = "unsupported-command-or-arguments"; return false;
}
struct Backend {
    virtual ~Backend() = default;
    virtual bool root(Widget& w, std::string& error) = 0;
    virtual bool roots(std::vector<Widget>& out, std::string& error) {
        Widget w = 0;
        if (!root(w, error)) return false;
        out.push_back(w); return true;
    }
    virtual bool node(Widget w, Node& n) = 0;
    virtual bool children(Widget w, std::vector<Widget>& out) = 0;
    virtual bool click(Widget w, std::string& error) = 0;
    virtual bool key(const std::string&, std::string& error) { error = "key-unavailable"; return false; }
    virtual bool text(Widget, const std::string&, std::string& error) { error = "text-unavailable"; return false; }
    virtual std::string players() { return fail("players-unavailable"); }
    virtual bool quit(std::string& error) { error = "quit-unavailable"; return false; }
};
inline bool resolve(Backend& b, Widget root, const std::string& path, Widget& result, std::string& error) {
    std::vector<std::string> parts;
    if (!pathParts(path, parts)) { error = "invalid-path"; return false; }
    std::vector<Widget> current{root};
    size_t visited = 0;
    std::unordered_set<Widget> seen{root};
    for (const auto& part : parts) {
        std::vector<Widget> next;
        for (auto w : current) {
            std::vector<Widget> children;
            if (!b.children(w, children)) { error = "unreadable-children"; return false; }
            bool syntheticMatch = false, namedMatch = false;
            for (size_t index = 0; index < children.size(); ++index) {
                auto child = children[index];
                if (++visited > MaxNodes) { error = "node-limit"; return false; }
                Node n;
                if (!seen.insert(child).second || !b.node(child, n)) { error = "invalid-or-cyclic-child"; return false; }
                if (childSegment(n.id, index) == part) {
                    if (addressableId(n.id)) namedMatch = true;
                    else syntheticMatch = true;
                    next.push_back(child);
                }
            }
            // A literal #N must never accidentally select the synthetic child (even deeper down).
            if (syntheticMatch && namedMatch) { error = "ambiguous-path"; return false; }
        }
        current = next;
        if (current.empty()) { error = "not-found"; return false; }
    }
    if (current.size() != 1) { error = "ambiguous-path"; return false; }
    result = current.front(); return true;
}
inline std::string execute(const Request& r, Backend& b, bool& armed, bool flagExists) {
    if (r.verb == "disarm") { armed = false; return "ok disarmed\n"; }
    if (r.verb == "status") return armed ? "ok armed\n" : "ok disarmed\n";
    if (r.verb == "arm") {
        // A failed re-arm must also revoke a previous arm.
        armed = flagExists;
        return armed ? "ok armed\n" : fail("arming-flag-missing");
    }
    if (!armed) return fail("disarmed");
    // Closing must work while the frontend root is being torn down, too.
    if (r.verb == "quit") {
        std::string error;
        return b.quit(error) ? "ok close-posted\n" : fail(error);
    }
    if (r.verb == "players") return b.players();
    // Esc must work during panel/root teardown, with the same arming rule as clicks.
    if (r.verb == "key") {
        std::string error;
        return b.key(r.value, error) ? "ok\n" : fail(error.empty() ? "input-rejected" : error);
    }
    Widget root = 0, target = 0; std::string error;
    std::vector<Widget> roots;
    if (!b.roots(roots, error)) return fail(error);
    if (roots.empty()) return fail("no-live-widget-root-source");
    if (roots.size() > MaxNodes) return fail("node-limit");
    std::unordered_set<Widget> rootSet;
    for (auto w : roots) if (!w || !rootSet.insert(w).second) return fail("invalid-or-duplicate-root");
    root = roots.front();
    if (r.verb == "roots") {
        std::string reply;
        for (size_t i = 0; i < roots.size(); ++i) {
            Node n; std::vector<Widget> children;
            if (!b.node(roots[i], n) || !b.children(roots[i], children)) return fail("unreadable-root");
            if (children.size() > MaxNodes) return fail("node-limit");
            if (roots.size() == 1) for (auto w : children) {
                Node child;
                if (!b.node(w, child)) return fail("invalid-child");
                if (child.id == "@0") return fail("root-selector-collides-with-legacy-id");
            }
            auto row = "root\t/@" + std::to_string(i) + "\t" + escaped(n.id) + "\t" + std::to_string(children.size()) + "\n";
            if (reply.size() + row.size() + 100 > MaxReply) return fail("reply-limit");
            reply += row;
        }
        return reply + "ok\n";
    }
    const bool forest = roots.size() > 1 && r.verb == "tree" && r.path == "/";
    if (r.verb != "find" && r.verb != "modal" && !forest) {
        std::vector<std::string> parts;
        if (!pathParts(r.path, parts)) return fail("invalid-path");
        std::string localPath = r.path;
        if (!parts.empty() && parts.front().size() > 1 && parts.front()[0] == '@' &&
            (roots.size() > 1 || parts.front() == "@0")) {
            const auto selector = parts.front();
            size_t index = 0;
            for (size_t c = 1; c < selector.size(); ++c) {
                if (selector[c] < '0' || selector[c] > '9' || index > MaxNodes) return fail("invalid-root-selector");
                index = index * 10 + selector[c] - '0';
            }
            if (selector != "@" + std::to_string(index) || index >= roots.size()) return fail("invalid-root-selector");
            // Single-root literal @0 retains its legacy meaning; roots refuses to advertise
            // an alias in that case. Other single-root @Ids never enter the namespace.
            bool legacy = false;
            if (roots.size() == 1) {
                std::vector<Widget> children;
                if (!b.children(root, children)) return fail("unreadable-children");
                if (children.size() > MaxNodes) return fail("node-limit");
                for (auto w : children) {
                    Node n;
                    if (!b.node(w, n)) return fail("invalid-child");
                    if (n.id == selector) legacy = true;
                }
            }
            if (!legacy) {
                root = roots[index];
                localPath = r.path.substr(selector.size() + 1);
                if (localPath.empty()) localPath = "/";
            }
        } else if (roots.size() != 1) return fail("ambiguous-root-use-/@N");
        if (!resolve(b, root, localPath, target, error)) return fail(error);
    }
    if (r.verb == "click" || r.verb == "text") {
        Node n;
        if (!b.node(target, n)) return fail("unreadable-widget");
        if (!n.visible) return fail("hidden");
        if (n.disabled) return fail("disabled");
        bool ok = r.verb == "click" ? b.click(target, error) : b.text(target, r.value, error);
        return ok ? "ok\n" : fail(error.empty() ? "input-rejected" : error);
    }
    struct Entry { Widget w; std::string path; int depth; };
    std::vector<Entry> pending;
    if (r.verb == "find" || r.verb == "modal" || forest) {
        for (size_t i = roots.size(); i > 0; --i)
            pending.push_back({roots[i - 1], roots.size() == 1 ? "/" : "/@" + std::to_string(i - 1), 0});
    } else pending.push_back({target, r.path, 0});
    std::unordered_set<Widget> seen;
    std::string reply;
    while (!pending.empty()) {
        auto e = pending.back(); pending.pop_back();
        if (seen.size() >= MaxNodes) return reply + fail("node-limit");
        if (!seen.insert(e.w).second) return reply + fail("cyclic-or-shared-child");
        Node n;
        if (!b.node(e.w, n)) return reply + fail("unreadable-widget");
        if (r.verb == "tree" || (r.verb == "find" && e.depth != 0 && n.id == r.path) ||
            (r.verb == "modal" && e.depth == 1 && n.visible)) {
            auto row = e.path + "\t" + (n.visible ? "1" : "0") + "\t" + (n.disabled ? "1" : "0") + "\t" + escaped(n.state) + "\t" + escaped(n.text) + (r.verb == "modal" || (e.path != "/" && !addressableId(n.id)) ? "\t" + escaped(n.id) : "") + "\n";
            if (reply.size() + row.size() + 100 > MaxReply) return reply + fail("reply-limit");
            reply += row;
        }
        if (r.verb == "tree" && e.depth >= r.depth) continue;
        if (r.verb == "modal" && e.depth >= 1) continue;
        if (e.depth >= 64) return reply + fail("traversal-depth-limit");
        std::vector<Widget> children;
        if (!b.children(e.w, children)) return reply + fail("unreadable-children");
        if (pending.size() + seen.size() + children.size() > MaxNodes) return reply + fail("node-limit");
        for (size_t index = children.size(); index > 0; --index) {
            auto w = children[index - 1];
            Node child;
            if (!b.node(w, child)) return reply + fail("invalid-child");
            auto segment = childSegment(child.id, index - 1);
            pending.push_back({w, e.path == "/" ? "/" + segment : e.path + "/" + segment, e.depth + 1});
        }
    }
    return reply + "ok\n";
}
} // namespace twui
