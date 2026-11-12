// Compiles the shader translator's HLSL dialect to DXBC (SM5) directly.
//
// Front end: a lexer with #define substitution and a recursive-descent parser for the subset the translator emits
// (globals, cbuffers, resources, functions, if/for/switch, the usual operators and intrinsics).
// Back end: straightforward code generation without optimisation. Every value lives in temp registers (scalars
// packed four to a register), user functions become subroutines (label/call) with their own temp pools, and
// asfloat/asuint are free because DXBC registers are typeless.
#include <cctype>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include "gpu/dxbc/dxbc.h"

namespace gpu::dxbc {
namespace {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};
[[noreturn]] void Fail(const std::string& m) { throw Error(m); }

// ================================================================ lexer
struct Token {
    enum K { End, Id, Num, Punct } k = End;
    std::string s;
    bool is_float = false, is_uint = false;
    u64 iv = 0;
    double fv = 0;
    int line = 0;
};

std::vector<Token> Lex(const std::string& src) {
    std::vector<Token> out;
    std::unordered_map<std::string, std::string> defines;
    size_t i = 0;
    int line = 1;
    const size_t n = src.size();
    auto peek = [&](size_t k) { return i + k < n ? src[i + k] : '\0'; };
    bool line_start = true;
    while (i < n) {
        const char c = src[i];
        if (c == '\n') {
            line++;
            i++;
            line_start = true;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            i++;
            continue;
        }
        if (c == '/' && peek(1) == '/') {
            while (i < n && src[i] != '\n')
                i++;
            continue;
        }
        if (c == '/' && peek(1) == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) {
                if (src[i] == '\n')
                    line++;
                i++;
            }
            i += 2;
            continue;
        }
        if (c == '#' && line_start) {
            size_t e = src.find('\n', i);
            if (e == std::string::npos)
                e = n;
            const std::string l = src.substr(i, e - i);
            if (l.rfind("#define", 0) == 0) {
                size_t p = 7;
                while (p < l.size() && l[p] == ' ')
                    p++;
                size_t q = p;
                while (q < l.size() && (isalnum(u8(l[q])) || l[q] == '_'))
                    q++;
                const std::string name = l.substr(p, q - p);
                while (q < l.size() && l[q] == ' ')
                    q++;
                std::string val = l.substr(q);
                while (!val.empty() && (val.back() == ' ' || val.back() == '\r'))
                    val.pop_back();
                defines[name] = val;
            }
            i = e;
            continue;
        }
        line_start = false;
        Token t;
        t.line = line;
        if (isalpha(u8(c)) || c == '_') {
            size_t q = i;
            while (q < n && (isalnum(u8(src[q])) || src[q] == '_'))
                q++;
            t.k = Token::Id;
            t.s = src.substr(i, q - i);
            i = q;
            if (auto d = defines.find(t.s); d != defines.end())
                t.s = d->second; // single-identifier substitutions only
            out.push_back(t);
            continue;
        }
        if (isdigit(u8(c)) || (c == '.' && isdigit(u8(peek(1))))) {
            size_t q = i;
            t.k = Token::Num;
            if (c == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
                q += 2;
                while (q < n && isxdigit(u8(src[q])))
                    q++;
                t.iv = std::stoull(src.substr(i + 2, q - i - 2), nullptr, 16);
            } else {
                bool flt = false;
                while (q < n && (isdigit(u8(src[q])) || src[q] == '.' || src[q] == 'e' || src[q] == 'E' ||
                                 ((src[q] == '-' || src[q] == '+') && (src[q - 1] == 'e' || src[q - 1] == 'E')))) {
                    if (src[q] == '.' || src[q] == 'e' || src[q] == 'E')
                        flt = true;
                    q++;
                }
                const std::string num = src.substr(i, q - i);
                if (flt) {
                    t.is_float = true;
                    t.fv = std::stod(num);
                } else {
                    t.iv = std::stoull(num);
                }
            }
            if (q < n && (src[q] == 'u' || src[q] == 'U')) {
                t.is_uint = true;
                q++;
            } else if (q < n && (src[q] == 'f' || src[q] == 'F')) {
                if (!t.is_float) {
                    t.is_float = true;
                    t.fv = double(t.iv);
                }
                q++;
            } else if (q < n && (src[q] == 'l' || src[q] == 'L')) {
                q++;
            }
            i = q;
            out.push_back(t);
            continue;
        }
        static const char* ops[] = {"<<=", ">>=", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "++", "--", "+=", "-=",
                                    "*=",  "/=",  "%=", "&=", "|=", "^="};
        t.k = Token::Punct;
        bool matched = false;
        for (const char* o : ops) {
            const size_t l = std::strlen(o);
            if (src.compare(i, l, o) == 0) {
                t.s = o;
                i += l;
                matched = true;
                break;
            }
        }
        if (!matched) {
            t.s = std::string(1, c);
            i++;
        }
        out.push_back(t);
    }
    Token end;
    end.line = line;
    out.push_back(end);
    return out;
}

// ================================================================ types and AST
enum class B : u8 { Void, Bool, Int, Uint, Float };
struct Ty {
    B b = B::Void;
    u8 n = 1;
    bool operator==(const Ty& o) const { return b == o.b && n == o.n; }
};

bool ParseScalarType(const std::string& s, Ty& t) {
    static const std::pair<const char*, B> bases[] = {{"bool", B::Bool}, {"int", B::Int}, {"uint", B::Uint}, {"float", B::Float},
                                                      {"half", B::Float}, {"dword", B::Uint}};
    for (auto& [name, b] : bases) {
        const size_t l = std::strlen(name);
        if (s.compare(0, l, name) != 0)
            continue;
        if (s.size() == l) {
            t = {b, 1};
            return true;
        }
        if (s.size() == l + 1 && s[l] >= '1' && s[l] <= '4') {
            t = {b, u8(s[l] - '0')};
            return true;
        }
    }
    if (s == "void") {
        t = {B::Void, 0};
        return true;
    }
    return false;
}

struct Expr;
using EP = std::unique_ptr<Expr>;
struct Expr {
    enum K { Lit, Name, Unary, Binary, Cond, Call, Method, Index, Member, Assign, IncDec } k;
    std::string op; // operator, name, swizzle or method
    std::vector<EP> a;
    Ty lit_ty;
    u32 lit = 0;
    bool post = false;
    int line = 0;
};

struct Stmt;
using SP = std::unique_ptr<Stmt>;
struct LocalDecl {
    Ty t;
    std::string name;
    u32 array = 0;
    EP init;
    std::string struct_name; // a struct-typed local (the entry point's output)
};
struct Stmt {
    enum K { ExprS, Decl, If, For, Switch, Break, Continue, Return, Block, Discard } k;
    EP e;                        // expression / condition / return value / selector
    EP inc;                      // for: increment
    std::vector<LocalDecl> decls;
    std::vector<SP> body;        // block items; if: then, else; for: init, body
    std::vector<std::pair<std::vector<s64>, std::vector<SP>>> cases; // switch (INT64_MIN = default)
    bool unroll = false;
    int line = 0;
};

struct Param {
    Ty t;
    std::string name, semantic;
    bool out = false, in = true;
    std::string struct_name; // a struct-typed parameter (the entry point's input)
};
// Vertex and pixel entry points take and return structs of semantic-tagged members.
struct StructMember {
    Ty t;
    std::string name, semantic;
    u32 interp = 0; // 0 default (linear), 1 nointerpolation, 3 noperspective
};
struct Func {
    std::string name;
    Ty ret;
    std::string ret_struct;
    std::vector<Param> params;
    SP body;
    u32 numthreads[3] = {1, 1, 1};
};

enum class ResKind { Texture, RawBuffer, RWRawBuffer, Sampler };
struct Resource {
    ResKind kind;
    std::string dim; // Buffer, Texture1D, Texture2D, ...
    Ty elem{B::Float, 4};
    u32 reg = 0;
    bool comparison = false;
};
struct CBField {
    u32 slot = 0, row = 0;
    Ty t;
    u32 array = 0;
};
struct Global {
    Ty t;
    u32 array = 0;
    bool groupshared = false;
    EP init;
};

// ================================================================ parser
class Parser {
public:
    explicit Parser(std::vector<Token> toks) : t_(std::move(toks)) {}

    std::map<std::string, Resource> resources;
    std::map<std::string, CBField> cbfields;
    std::map<std::string, u32> cbuffer_rows; // slot -> rows (by name "CBn")
    std::map<u32, u32> cb_size;              // slot -> rows
    std::vector<std::pair<std::string, Global>> globals;
    std::map<std::string, Func> funcs;
    std::map<std::string, std::vector<StructMember>> structs;

    void ParseProgram() {
        while (Cur().k != Token::End) {
            if (Is("cbuffer")) {
                ParseCBuffer();
                continue;
            }
            if (Is("struct")) {
                ParseStruct();
                continue;
            }
            std::vector<std::string> attrs;
            u32 nt[3] = {1, 1, 1};
            while (Is("[")) {
                Next();
                const std::string a = Ident();
                if (a == "numthreads") {
                    Expect("(");
                    for (int k = 0; k < 3; k++) {
                        nt[k] = u32(Number());
                        if (k < 2)
                            Expect(",");
                    }
                    Expect(")");
                } else if (Is("(")) {
                    SkipParens();
                }
                Expect("]");
                attrs.push_back(a);
            }
            bool is_static = false, groupshared = false;
            while (Is("static") || Is("groupshared") || Is("const")) {
                if (Is("static"))
                    is_static = true;
                if (Is("groupshared"))
                    groupshared = true;
                Next();
            }
            const std::string tname = Ident();
            if (IsResourceType(tname)) {
                ParseResource(tname);
                continue;
            }
            Ty ty;
            const bool struct_ret = structs.count(tname) != 0;
            if (struct_ret)
                ty = {B::Void, 0};
            else if (!ParseScalarType(tname, ty))
                Fail("unknown type " + tname + " at line " + std::to_string(Cur().line));
            const std::string name = Ident();
            if (Is("(")) { // function
                Func f;
                f.name = name;
                f.ret = ty;
                if (struct_ret)
                    f.ret_struct = tname;
                std::memcpy(f.numthreads, nt, sizeof(nt));
                Next();
                while (!Is(")")) {
                    Param p;
                    while (Is("in") || Is("out") || Is("inout")) {
                        if (Is("out"))
                            p.out = true, p.in = false;
                        if (Is("inout"))
                            p.out = true;
                        Next();
                    }
                    const std::string ptype = Ident();
                    if (structs.count(ptype))
                        p.struct_name = ptype;
                    else if (!ParseScalarType(ptype, p.t))
                        Fail("bad parameter type in " + name);
                    p.name = Ident();
                    if (Is(":")) {
                        Next();
                        p.semantic = Ident();
                    }
                    f.params.push_back(p);
                    if (Is(","))
                        Next();
                }
                Next();
                f.body = ParseBlock();
                funcs[name] = std::move(f);
                continue;
            }
            // global variable(s)
            for (std::string vname = name;;) {
                Global g;
                g.t = ty;
                g.groupshared = groupshared;
                if (Is("[")) {
                    Next();
                    g.array = u32(Number());
                    Expect("]");
                }
                if (Is("=")) {
                    Next();
                    g.init = ParseAssign();
                }
                globals.push_back({vname, std::move(g)});
                if (!Is(","))
                    break;
                Next();
                vname = Ident();
            }
            (void)is_static;
            Expect(";");
        }
    }

private:
    std::vector<Token> t_;
    size_t p_ = 0;

    const Token& Cur() const { return t_[p_]; }
    const Token& At(size_t k) const { return t_[std::min(p_ + k, t_.size() - 1)]; }
    bool Is(const char* s) const { return Cur().k != Token::End && Cur().k != Token::Num && Cur().s == s; }
    void Next() {
        if (p_ + 1 < t_.size())
            p_++;
    }
    void Expect(const char* s) {
        if (!Is(s))
            Fail(std::string("expected '") + s + "' at line " + std::to_string(Cur().line) + " near '" + Cur().s + "'");
        Next();
    }
    std::string Ident() {
        if (Cur().k != Token::Id)
            Fail("expected identifier at line " + std::to_string(Cur().line) + " near '" + Cur().s + "'");
        std::string s = Cur().s;
        Next();
        return s;
    }
    s64 Number() {
        bool neg = false;
        if (Is("-")) {
            neg = true;
            Next();
        }
        if (Cur().k != Token::Num)
            Fail("expected number at line " + std::to_string(Cur().line));
        const s64 v = s64(Cur().iv);
        Next();
        return neg ? -v : v;
    }
    void SkipParens() {
        int depth = 0;
        do {
            if (Is("("))
                depth++;
            if (Is(")"))
                depth--;
            Next();
        } while (depth > 0 && Cur().k != Token::End);
    }
    static bool IsResourceType(const std::string& s) {
        return s == "Buffer" || s.rfind("Texture", 0) == 0 || s == "ByteAddressBuffer" || s == "RWByteAddressBuffer" ||
               s == "SamplerState" || s == "SamplerComparisonState";
    }
    u32 Register() { // : register(t3)
        Expect(":");
        if (Ident() != "register")
            Fail("expected register()");
        Expect("(");
        const std::string r = Ident();
        Expect(")");
        return u32(std::stoul(r.substr(1)));
    }
    void ParseResource(const std::string& tname) {
        Resource r;
        r.dim = tname;
        if (tname == "SamplerState" || tname == "SamplerComparisonState") {
            r.kind = ResKind::Sampler;
            r.comparison = tname == "SamplerComparisonState";
        } else if (tname == "ByteAddressBuffer") {
            r.kind = ResKind::RawBuffer;
        } else if (tname == "RWByteAddressBuffer") {
            r.kind = ResKind::RWRawBuffer;
        } else {
            r.kind = ResKind::Texture;
            if (Is("<")) {
                Next();
                if (!ParseScalarType(Ident(), r.elem))
                    Fail("bad texture element type");
                Expect(">");
            }
        }
        const std::string name = Ident();
        r.reg = Register();
        Expect(";");
        resources[name] = r;
    }
    void ParseStruct() {
        Next();
        const std::string sname = Ident();
        Expect("{");
        std::vector<StructMember> members;
        while (!Is("}")) {
            StructMember m;
            while (Is("nointerpolation") || Is("noperspective") || Is("linear") || Is("centroid")) {
                if (Is("nointerpolation"))
                    m.interp = 1;
                else if (Is("noperspective"))
                    m.interp = 3;
                else if (Is("centroid"))
                    Fail("centroid interpolation");
                Next();
            }
            if (!ParseScalarType(Ident(), m.t))
                Fail("bad struct member type in " + sname);
            m.name = Ident();
            if (Is(":")) {
                Next();
                m.semantic = Ident();
            }
            Expect(";");
            members.push_back(m);
        }
        Next();
        Expect(";");
        structs[sname] = std::move(members);
    }
    void ParseCBuffer() {
        Next();
        Ident(); // block name
        const u32 slot = Register();
        Expect("{");
        u32 row = 0;
        while (!Is("}")) {
            Ty t;
            if (!ParseScalarType(Ident(), t))
                Fail("bad cbuffer member type");
            CBField f;
            f.slot = slot;
            f.t = t;
            const std::string name = Ident();
            if (Is("[")) {
                Next();
                f.array = u32(Number());
                Expect("]");
            }
            f.row = row;
            row += std::max<u32>(f.array, 1); // the translator only declares 16-byte vector members
            Expect(";");
            cbfields[name] = f;
        }
        Next();
        if (Is(";"))
            Next();
        cb_size[slot] = row;
    }

    // ---------------- statements
    SP ParseBlock() {
        auto s = std::make_unique<Stmt>();
        s->k = Stmt::Block;
        s->line = Cur().line;
        Expect("{");
        while (!Is("}"))
            s->body.push_back(ParseStmt());
        Next();
        return s;
    }
    bool AtType() const {
        Ty t;
        return Cur().k == Token::Id && (ParseScalarType(Cur().s, t) || structs.count(Cur().s)) && At(1).k == Token::Id;
    }
    SP ParseStmt() {
        const int line = Cur().line;
        bool unroll = false;
        while (Is("[")) {
            Next();
            const std::string a = Ident();
            if (a == "unroll")
                unroll = true;
            if (Is("("))
                SkipParens();
            Expect("]");
        }
        if (Is("{"))
            return ParseBlock();
        auto s = std::make_unique<Stmt>();
        s->line = line;
        s->unroll = unroll;
        if (Is(";")) {
            Next();
            s->k = Stmt::Block;
            return s;
        }
        if (Is("if")) {
            Next();
            s->k = Stmt::If;
            Expect("(");
            s->e = ParseExpr();
            Expect(")");
            s->body.push_back(ParseStmt());
            if (Is("else")) {
                Next();
                s->body.push_back(ParseStmt());
            }
            return s;
        }
        if (Is("for")) {
            Next();
            s->k = Stmt::For;
            Expect("(");
            if (Is(";")) {
                Next();
                s->body.push_back(nullptr);
            } else {
                s->body.push_back(ParseSimple());
                Expect(";");
            }
            if (!Is(";"))
                s->e = ParseExpr();
            Expect(";");
            if (!Is(")"))
                s->inc = ParseExpr();
            Expect(")");
            s->body.push_back(ParseStmt());
            return s;
        }
        if (Is("switch")) {
            Next();
            s->k = Stmt::Switch;
            Expect("(");
            s->e = ParseExpr();
            Expect(")");
            Expect("{");
            while (!Is("}")) {
                std::vector<s64> labels;
                while (Is("case") || Is("default")) {
                    if (Is("default")) {
                        Next();
                        labels.push_back(INT64_MIN);
                    } else {
                        Next();
                        labels.push_back(Number());
                    }
                    Expect(":");
                }
                if (labels.empty())
                    Fail("statement outside a case at line " + std::to_string(Cur().line));
                std::vector<SP> body;
                while (!Is("case") && !Is("default") && !Is("}"))
                    body.push_back(ParseStmt());
                s->cases.push_back({labels, std::move(body)});
            }
            Next();
            return s;
        }
        if (Is("break")) {
            Next();
            Expect(";");
            s->k = Stmt::Break;
            return s;
        }
        if (Is("continue")) {
            Next();
            Expect(";");
            s->k = Stmt::Continue;
            return s;
        }
        if (Is("discard")) {
            Next();
            Expect(";");
            s->k = Stmt::Discard;
            return s;
        }
        if (Is("return")) {
            Next();
            s->k = Stmt::Return;
            if (!Is(";"))
                s->e = ParseExpr();
            Expect(";");
            return s;
        }
        auto r = ParseSimple();
        Expect(";");
        return r;
    }
    // declaration or expression (no trailing ';')
    SP ParseSimple() {
        auto s = std::make_unique<Stmt>();
        s->line = Cur().line;
        while (Is("const"))
            Next();
        if (AtType()) {
            s->k = Stmt::Decl;
            Ty t;
            const std::string tname = Ident();
            const bool is_struct = structs.count(tname) != 0;
            if (!is_struct)
                ParseScalarType(tname, t);
            for (;;) {
                LocalDecl d;
                d.t = t;
                if (is_struct)
                    d.struct_name = tname;
                d.name = Ident();
                if (Is("[")) {
                    Next();
                    d.array = u32(Number());
                    Expect("]");
                }
                if (Is("=")) {
                    Next();
                    d.init = ParseAssign();
                }
                s->decls.push_back(std::move(d));
                if (!Is(","))
                    break;
                Next();
            }
            return s;
        }
        s->k = Stmt::ExprS;
        s->e = ParseExpr();
        return s;
    }

    // ---------------- expressions
    EP Make(Expr::K k, const std::string& op = {}) {
        auto e = std::make_unique<Expr>();
        e->k = k;
        e->op = op;
        e->line = Cur().line;
        return e;
    }
    EP ParseExpr() { return ParseAssign(); }
    EP ParseAssign() {
        EP l = ParseCond();
        static const char* ops[] = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="};
        for (const char* o : ops)
            if (Is(o)) {
                Next();
                auto e = Make(Expr::Assign, o);
                e->a.push_back(std::move(l));
                e->a.push_back(ParseAssign());
                return e;
            }
        return l;
    }
    EP ParseCond() {
        EP c = ParseBin(0);
        if (!Is("?"))
            return c;
        Next();
        auto e = Make(Expr::Cond);
        e->a.push_back(std::move(c));
        e->a.push_back(ParseAssign());
        Expect(":");
        e->a.push_back(ParseAssign());
        return e;
    }
    static int Prec(const std::string& op) {
        static const std::map<std::string, int> p = {{"||", 1}, {"&&", 2}, {"|", 3},  {"^", 4},  {"&", 5},  {"==", 6}, {"!=", 6},
                                                     {"<", 7},  {">", 7},  {"<=", 7}, {">=", 7}, {"<<", 8}, {">>", 8}, {"+", 9},
                                                     {"-", 9},  {"*", 10}, {"/", 10}, {"%", 10}};
        auto it = p.find(op);
        return it == p.end() ? -1 : it->second;
    }
    EP ParseBin(int min_prec) {
        EP l = ParseUnary();
        for (;;) {
            if (Cur().k != Token::Punct)
                break;
            const std::string op = Cur().s;
            const int pr = Prec(op);
            if (pr < 0 || pr < min_prec)
                break;
            Next();
            EP r = ParseBin(pr + 1);
            auto e = Make(Expr::Binary, op);
            e->a.push_back(std::move(l));
            e->a.push_back(std::move(r));
            l = std::move(e);
        }
        return l;
    }
    EP ParseUnary() {
        if (Is("-") || Is("+") || Is("!") || Is("~")) {
            const std::string op = Cur().s;
            Next();
            auto e = Make(Expr::Unary, op);
            e->a.push_back(ParseUnary());
            return e;
        }
        if (Is("++") || Is("--")) {
            const std::string op = Cur().s;
            Next();
            auto e = Make(Expr::IncDec, op);
            e->a.push_back(ParseUnary());
            return e;
        }
        return ParsePostfix();
    }
    EP ParsePostfix() {
        EP e = ParsePrimary();
        for (;;) {
            if (Is("[")) {
                Next();
                auto x = Make(Expr::Index);
                x->a.push_back(std::move(e));
                x->a.push_back(ParseExpr());
                Expect("]");
                e = std::move(x);
            } else if (Is(".")) {
                Next();
                const std::string m = Ident();
                if (Is("(")) {
                    auto x = Make(Expr::Method, m);
                    x->a.push_back(std::move(e));
                    Next();
                    while (!Is(")")) {
                        x->a.push_back(ParseAssign());
                        if (Is(","))
                            Next();
                    }
                    Next();
                    e = std::move(x);
                } else {
                    auto x = Make(Expr::Member, m);
                    x->a.push_back(std::move(e));
                    e = std::move(x);
                }
            } else if (Is("++") || Is("--")) {
                auto x = Make(Expr::IncDec, Cur().s);
                x->post = true;
                Next();
                x->a.push_back(std::move(e));
                e = std::move(x);
            } else {
                break;
            }
        }
        return e;
    }
    EP ParsePrimary() {
        if (Is("(")) {
            Next();
            EP e = ParseExpr();
            Expect(")");
            return e;
        }
        if (Cur().k == Token::Num) {
            auto e = Make(Expr::Lit);
            const Token& t = Cur();
            if (t.is_float) {
                const float f = float(t.fv);
                std::memcpy(&e->lit, &f, 4);
                e->lit_ty = {B::Float, 1};
            } else {
                e->lit = u32(t.iv);
                e->lit_ty = {t.is_uint ? B::Uint : B::Int, 1};
            }
            Next();
            return e;
        }
        if (Is("true") || Is("false")) {
            auto e = Make(Expr::Lit);
            e->lit = Is("true") ? 0xFFFFFFFFu : 0u;
            e->lit_ty = {B::Bool, 1};
            Next();
            return e;
        }
        const std::string name = Ident();
        if (Is("(")) {
            auto e = Make(Expr::Call, name);
            Next();
            while (!Is(")")) {
                e->a.push_back(ParseAssign());
                if (Is(","))
                    Next();
            }
            Next();
            return e;
        }
        return Make(Expr::Name, name);
    }
};

// ================================================================ code generation
// D3D10/11 tokenized program format constants.
enum Opcode : u32 {
    OP_ADD = 0, OP_AND = 1, OP_BREAK = 2, OP_CALL = 4, OP_CASE = 6, OP_CONTINUE = 7, OP_DEFAULT = 10, OP_DIV = 14, OP_ELSE = 18,
    OP_ENDIF = 21, OP_ENDLOOP = 22, OP_ENDSWITCH = 23, OP_EQ = 24, OP_EXP = 25, OP_FRC = 26, OP_FTOI = 27, OP_FTOU = 28,
    OP_GE = 29, OP_IADD = 30, OP_IF = 31, OP_IEQ = 32, OP_IGE = 33, OP_ILT = 34, OP_IMAD = 35, OP_IMAX = 36, OP_IMIN = 37,
    OP_IMUL = 38, OP_INE = 39, OP_INEG = 40, OP_ISHL = 41, OP_ISHR = 42, OP_ITOF = 43, OP_LABEL = 44, OP_LD = 45, OP_LOG = 47,
    OP_LOOP = 48, OP_LT = 49, OP_MAD = 50, OP_MIN = 51, OP_MAX = 52, OP_MOV = 54, OP_MOVC = 55, OP_MUL = 56, OP_NE = 57,
    OP_NOT = 59, OP_OR = 60, OP_RESINFO = 61, OP_RET = 62, OP_ROUND_NE = 64, OP_ROUND_NI = 65, OP_ROUND_PI = 66,
    OP_ROUND_Z = 67, OP_RSQ = 68, OP_SAMPLE_C_LZ = 71, OP_SAMPLE_L = 72, OP_SQRT = 75, OP_SWITCH = 76, OP_SINCOS = 77,
    OP_UDIV = 78, OP_ULT = 79, OP_UGE = 80, OP_UMUL = 81, OP_UMAD = 82, OP_UMAX = 83, OP_UMIN = 84, OP_USHR = 85, OP_UTOF = 86,
    OP_XOR = 87, OP_DCL_RESOURCE = 88, OP_DCL_CONSTANT_BUFFER = 89, OP_DCL_SAMPLER = 90, OP_DCL_INPUT = 95,
    OP_DCL_TEMPS = 104, OP_DCL_INDEXABLE_TEMP = 105, OP_DCL_GLOBAL_FLAGS = 106, OP_GATHER4 = 109, OP_GATHER4_C = 126,
    OP_F32TOF16 = 130, OP_F16TOF32 = 131, OP_COUNTBITS = 134, OP_FIRSTBIT_HI = 135, OP_FIRSTBIT_LO = 136, OP_BFREV = 141,
    OP_DCL_THREAD_GROUP = 155, OP_DCL_UAV_RAW = 157, OP_DCL_TGSM_STRUCTURED = 160, OP_DCL_RESOURCE_RAW = 161,
    OP_LD_RAW = 165, OP_STORE_RAW = 166, OP_LD_STRUCTURED = 167, OP_STORE_STRUCTURED = 168, OP_ATOMIC_AND = 169,
    OP_ATOMIC_OR = 170, OP_ATOMIC_XOR = 171, OP_ATOMIC_IADD = 173, OP_ATOMIC_UMAX = 176, OP_ATOMIC_UMIN = 177,
    OP_IMM_ATOMIC_IADD = 180, OP_IMM_ATOMIC_AND = 181, OP_IMM_ATOMIC_OR = 182, OP_IMM_ATOMIC_XOR = 183,
    OP_IMM_ATOMIC_EXCH = 184, OP_IMM_ATOMIC_IMAX = 186, OP_IMM_ATOMIC_IMIN = 187, OP_IMM_ATOMIC_UMAX = 188,
    OP_IMM_ATOMIC_UMIN = 189, OP_SYNC = 190,
    OP_DERIV_RTX = 11, OP_DERIV_RTY = 12, OP_DISCARD = 13, OP_SAMPLE = 69, OP_SAMPLE_C = 70, OP_SAMPLE_D = 73,
    OP_SAMPLE_B = 74, OP_LOD = 108, OP_DCL_INPUT_SGV = 96, OP_DCL_INPUT_PS = 98, OP_DCL_INPUT_PS_SGV = 99,
    OP_DCL_INPUT_PS_SIV = 100, OP_DCL_OUTPUT = 101, OP_DCL_OUTPUT_SIV = 103,
};
enum OperandType : u32 {
    OT_TEMP = 0, OT_INDEXABLE_TEMP = 3, OT_IMM32 = 4, OT_SAMPLER = 6, OT_RESOURCE = 7, OT_CB = 8, OT_LABEL = 10, OT_NULL = 13,
    OT_INPUT = 1, OT_OUTPUT = 2, OT_OUTPUT_DEPTH = 12, OT_UAV = 30, OT_TGSM = 31, OT_THREAD_ID = 32, OT_THREAD_GROUP_ID = 33, OT_THREAD_ID_IN_GROUP = 34,
    OT_THREAD_ID_IN_GROUP_FLATTENED = 35,
};
constexpr u32 kSaturate = 1u << 13;
constexpr u32 kTestNonZero = 1u << 18;

// A source operand.
struct Opnd {
    enum K { None, Temp, Imm, CB, Input, ITemp, InReg } k = None; // InReg: v# (reg = register)
    u32 reg = 0;        // temp index, input operand type, cb slot, indexable temp array
    u32 idx = 0;        // cb row / indexable element (immediate part)
    int rel = -1;       // relative index: temp register
    u8 rel_comp = 0;
    u8 swz[4] = {0, 1, 2, 3};
    u32 imm[4] = {};
    bool neg = false, abs = false;
};
struct Val {
    Ty t;
    Opnd o;
};

// Where a variable lives.
struct VarLoc {
    Ty t;
    enum K { Temp, ITemp, Tgsm, Const, Input, InReg } k = Temp; // InReg: entry-point input register v#
    u32 reg = 0;        // temp register / indexable array / input type
    u8 comp = 0;        // first component (scalars packed four to a register)
    u32 array = 0;      // element count for arrays
    Val constant;       // unrolled loop counters
};

class Codegen {
public:
    Codegen(Parser& p, Stage stage) : p_(p), stage_(stage) {}
    std::vector<SigElement> isgn, osgn; // filled by Run (vertex / pixel)

    std::vector<u32> Run() {
        auto mit = p_.funcs.find("main");
        if (mit == p_.funcs.end())
            Fail("no main");
        const Func& main = mit->second;
        // Statics: one shared pool every function can see.
        for (auto& [name, g] : p_.globals) {
            VarLoc v;
            v.t = g.t;
            if (g.groupshared) {
                if (g.t.b == B::Float || g.t.n != 1)
                    Fail("groupshared " + name + ": only uint arrays");
                v.k = VarLoc::Tgsm;
                v.reg = tgsm_count_++;
                v.array = std::max<u32>(g.array, 1);
                tgsm_.push_back(v.array);
            } else if (g.array) {
                v.k = VarLoc::ITemp;
                v.reg = u32(itemps_.size());
                v.array = g.array;
                itemps_.push_back({g.array, g.t.n});
            } else {
                AllocVar(v, true);
            }
            globals_[name] = v;
        }
        // Main.
        cur_pool_ = &pools_.emplace_back();
        scopes_.emplace_back();
        for (auto& prm : main.params) {
            if (!prm.struct_name.empty()) { // vertex / pixel inputs: member i in v<i>
                if (stage_ == Stage::Compute || !in_members_.empty())
                    Fail("struct parameter");
                in_members_ = p_.structs.at(prm.struct_name);
                for (u32 i = 0; i < in_members_.size(); i++) {
                    VarLoc v;
                    v.t = in_members_[i].t;
                    v.k = VarLoc::InReg;
                    v.reg = i;
                    scopes_.back()[prm.name + "." + in_members_[i].name] = v;
                }
                continue;
            }
            VarLoc v;
            v.t = prm.t;
            v.k = VarLoc::Input;
            if (prm.semantic == "SV_GroupThreadID")
                v.reg = OT_THREAD_ID_IN_GROUP;
            else if (prm.semantic == "SV_GroupID")
                v.reg = OT_THREAD_GROUP_ID;
            else if (prm.semantic == "SV_DispatchThreadID")
                v.reg = OT_THREAD_ID;
            else if (prm.semantic == "SV_GroupIndex")
                v.reg = OT_THREAD_ID_IN_GROUP_FLATTENED;
            else
                Fail("unsupported semantic " + prm.semantic);
            inputs_.insert(v.reg);
            scopes_.back()[prm.name] = v;
        }
        // Static initialisers (zero by default, as HLSL statics are).
        for (auto& [name, g] : p_.globals) {
            const VarLoc& v = globals_[name];
            if (v.k != VarLoc::Temp)
                continue;
            Val init = g.init ? Eval(g.init.get()) : Imm(v.t, 0);
            Store(v, Convert(init, v.t), nullptr);
            EndStatement();
        }
        in_main_ = true;
        main_ret_struct_ = main.ret_struct;
        if (stage_ != Stage::Compute && main.ret_struct.empty())
            Fail("entry point without an output struct");
        // The final call in main (the translator's Run()) is inlined: barriers inside subroutines are not handled
        // reliably by drivers, and any return inside it can simply end the shader.
        const Stmt* body = main.body.get();
        const Stmt* tail = body->k == Stmt::Block && !body->body.empty() ? body->body.back().get() : nullptr;
        const bool inline_tail = tail && tail->k == Stmt::ExprS && tail->e->k == Expr::Call &&
                                 p_.funcs.count(tail->e->op) && tail->e->a.empty() &&
                                 p_.funcs.at(tail->e->op).ret.b == B::Void;
        // Returns become breaks out of a one-iteration loop: a `ret` nested in flow control makes drivers drop the
        // enclosing work, so the program has a single `ret` at the end.
        Frame fr;
        fr.loop_base = loops_.size();
        fr.flag.t = {B::Bool, 1};
        AllocVar(fr.flag, false);
        Store(fr.flag, Imm({B::Bool, 1}, 0), nullptr);
        frames_.push_back(fr);
        Emit(OP_LOOP, {});
        if (inline_tail) {
            scopes_.emplace_back();
            for (size_t i = 0; i + 1 < body->body.size(); i++)
                CompileStmt(body->body[i].get());
            CompileStmt(p_.funcs.at(tail->e->op).body.get());
            scopes_.pop_back();
        } else {
            CompileStmt(body);
        }
        Emit(OP_BREAK, {});
        Emit(OP_ENDLOOP, {});
        frames_.pop_back();
        WriteOutputs();
        Emit(OP_RET, {});
        scopes_.pop_back();
        in_main_ = false;
        // Subroutines, compiled as they are discovered.
        while (!queue_.empty()) {
            FuncInfo* fi = queue_.back();
            queue_.pop_back();
            CompileFunc(*fi);
        }
        return Assemble(main);
    }

private:
    Parser& p_;
    std::vector<u32> code_;
    std::map<std::string, VarLoc> globals_;
    std::vector<std::map<std::string, VarLoc>> scopes_;
    struct ITempDecl {
        u32 count, comps;
    };
    std::vector<ITempDecl> itemps_;
    std::vector<u32> tgsm_;
    u32 tgsm_count_ = 0;
    std::set<u32> inputs_;
    Stage stage_;
    std::vector<StructMember> in_members_;  // entry-point input struct (v0, v1, ...)
    std::vector<StructMember> out_members_; // entry-point output struct
    std::vector<VarLoc> out_vars_;          // its members' temps, copied to the outputs before `ret`
    std::string main_ret_struct_;
    struct Pool;
    struct FuncInfo {
        const Func* f = nullptr;
        u32 label = 0;
        std::vector<VarLoc> params;
        VarLoc ret;
        Pool* pool = nullptr;
        bool queued = false;
    };
    std::map<std::string, FuncInfo> fn_;
    std::vector<FuncInfo*> queue_;
    std::set<std::string> used_resources_;
    std::map<u32, bool> cb_dynamic_;
    u32 next_label_ = 0;
    bool in_main_ = false;
    FuncInfo* cur_fn_ = nullptr;

    // Temp pools: statics first, then one pool per function so subroutines never clobber a caller's temps.
    struct Pool {
        std::vector<u32> free;      // scoped expression temps available
        std::vector<u32> scoped;    // allocated in the current statement
        u32 packed_reg = ~0u;       // register being filled with packed scalar variables
        u8 packed_used = 4;
    };
    std::deque<Pool> pools_; // deque: pools are referenced by pointer while more are added
    Pool* cur_pool_ = nullptr;
    Pool statics_;
    u32 next_reg_ = 0; // registers are never shared between pools
    u32 TempCount() const { return next_reg_; }
    u32 NewReg(Pool&) { return next_reg_++; }
    // Variable storage. Scalars share registers; vectors take a whole one.
    void AllocVar(VarLoc& v, bool is_static) {
        Pool& pl = is_static ? statics_ : *cur_pool_;
        v.k = VarLoc::Temp;
        if (v.t.n == 1) {
            if (pl.packed_used >= 4) {
                pl.packed_reg = NewReg(pl);
                pl.packed_used = 0;
            }
            v.reg = pl.packed_reg;
            v.comp = pl.packed_used++;
        } else {
            v.reg = NewReg(pl);
            v.comp = 0;
        }
    }
    u32 Scratch() {
        Pool& pl = *cur_pool_;
        u32 r;
        if (!pl.free.empty()) {
            r = pl.free.back();
            pl.free.pop_back();
        } else {
            r = NewReg(pl);
        }
        pl.scoped.push_back(r);
        return r;
    }
    void EndStatement() {
        Pool& pl = *cur_pool_;
        for (u32 r : pl.scoped)
            pl.free.push_back(r);
        pl.scoped.clear();
    }

    // ---------------- token emission
    static u32 SwzBits(const u8 s[4]) { return u32(s[0]) | (u32(s[1]) << 2) | (u32(s[2]) << 4) | (u32(s[3]) << 6); }
    // Source operand tokens. `scalar` selects one component (select_1) as FXC does for scalar sources.
    void PutSrc(std::vector<u32>& t, const Opnd& o, bool scalar) {
        if (o.k == Opnd::Imm && (o.neg || o.abs))
            Fail("internal: modifier on an immediate");
        if (o.k == Opnd::Imm) {
            if (scalar) {
                t.push_back(0x00004001);
                t.push_back(o.imm[o.swz[0]]);
            } else {
                t.push_back(0x00004002);
                for (int c = 0; c < 4; c++)
                    t.push_back(o.imm[o.swz[c]]);
            }
            return;
        }
        u32 tok = 2; // four components
        tok |= scalar ? ((2u << 2) | (u32(o.swz[0]) << 4)) : ((1u << 2) | (SwzBits(o.swz) << 4));
        u32 type = 0, dim = 0;
        switch (o.k) {
        case Opnd::Temp: type = OT_TEMP, dim = 1; break;
        case Opnd::Input: type = o.reg, dim = 0; break;
        case Opnd::InReg: type = OT_INPUT, dim = 1; break;
        case Opnd::CB: type = OT_CB, dim = 2; break;
        case Opnd::ITemp: type = OT_INDEXABLE_TEMP, dim = 2; break;
        default: Fail("bad source operand");
        }
        tok |= (type << 12) | (dim << 20);
        if (dim == 2 && o.rel >= 0)
            tok |= 3u << 25; // index 1: immediate + relative
        const bool mod = o.neg || o.abs;
        if (mod)
            tok |= 0x80000000u;
        t.push_back(tok);
        if (mod)
            t.push_back(1u | ((o.neg && o.abs ? 3u : o.abs ? 2u : 1u) << 6));
        if (o.k == Opnd::Temp || o.k == Opnd::InReg) {
            t.push_back(o.reg);
        } else if (dim == 2) {
            t.push_back(o.reg);
            t.push_back(o.idx);
            if (o.rel >= 0) {
                t.push_back(2u | (2u << 2) | (u32(o.rel_comp) << 4) | (OT_TEMP << 12) | (1u << 20));
                t.push_back(u32(o.rel));
            }
        }
    }
    struct Dst {
        u32 type = OT_TEMP;
        u32 reg = 0;
        u8 mask = 1;
        u32 idx = 0; // indexable temp element
        int rel = -1;
        u8 rel_comp = 0;
    };
    void PutDst(std::vector<u32>& t, const Dst& d) {
        if (d.type == OT_NULL) {
            t.push_back(OT_NULL << 12);
            return;
        }
        if (d.type == OT_OUTPUT_DEPTH) { // one component, no index
            t.push_back(1u | (OT_OUTPUT_DEPTH << 12));
            return;
        }
        u32 tok = 2 | (u32(d.mask) << 4) | (d.type << 12);
        if (d.type == OT_INDEXABLE_TEMP) {
            tok |= 2u << 20;
            if (d.rel >= 0)
                tok |= 3u << 25;
            t.push_back(tok);
            t.push_back(d.reg);
            t.push_back(d.idx);
            if (d.rel >= 0) {
                t.push_back(2u | (2u << 2) | (u32(d.rel_comp) << 4) | (OT_TEMP << 12) | (1u << 20));
                t.push_back(u32(d.rel));
            }
            return;
        }
        tok |= 1u << 20;
        t.push_back(tok);
        t.push_back(d.reg);
    }
    // Generic instruction: operands already encoded.
    void EmitRaw(u32 op, u32 ctrl, const std::vector<u32>& ops, const std::vector<u32>& ext = {}) {
        const u32 len = 1 + u32(ext.size()) + u32(ops.size());
        code_.push_back(op | ctrl | (len << 24) | (ext.empty() ? 0 : 0x80000000u));
        for (u32 e : ext)
            code_.push_back(e);
        for (u32 v : ops)
            code_.push_back(v);
    }
    void Emit(u32 op, const std::vector<u32>& ops, u32 ctrl = 0) { EmitRaw(op, ctrl, ops); }

    // ---------------- values
    static Val Imm(Ty t, u32 v) {
        Val r;
        r.t = t;
        r.o.k = Opnd::Imm;
        for (int c = 0; c < 4; c++)
            r.o.imm[c] = v;
        return r;
    }
    static Val ImmF(float f) {
        u32 b;
        std::memcpy(&b, &f, 4);
        return Imm({B::Float, 1}, b);
    }
    static Val TempVal(Ty t, u32 reg) {
        Val v;
        v.t = t;
        v.o.k = Opnd::Temp;
        v.o.reg = reg;
        return v;
    }
    // Component c of value v as an operand selecting that component.
    static Opnd Comp(const Val& v, int c) {
        Opnd o = v.o;
        const u8 s = o.swz[v.t.n == 1 ? 0 : c];
        for (int k = 0; k < 4; k++)
            o.swz[k] = s;
        return o;
    }
    static Val Swizzle(const Val& v, const u8* comps, int n) {
        Val r = v;
        r.t.n = u8(n);
        u8 s[4];
        for (int k = 0; k < 4; k++)
            s[k] = v.o.swz[v.t.n == 1 ? 0 : comps[std::min(k, n - 1)]];
        std::memcpy(r.o.swz, s, 4);
        return r;
    }
    // Emits `op dst.mask, srcs...` into a new temp holding a value of type t (n components).
    Val Op(u32 op, Ty t, std::initializer_list<Val> srcs, u32 ctrl = 0) {
        const u32 r = Scratch();
        std::vector<u32> tok;
        PutDst(tok, {OT_TEMP, r, u8((1u << t.n) - 1)});
        for (const Val& s : srcs)
            PutSrc(tok, Spread(s, t.n), t.n == 1);
        Emit(op, tok, ctrl);
        return TempVal(t, r);
    }
    // Source swizzle feeding destination components 0..n-1 (dst mask xyzw[0..n)).
    static Opnd Spread(const Val& v, int n) {
        Opnd o = v.o;
        if (v.t.n == 1)
            for (int k = 1; k < 4; k++)
                o.swz[k] = o.swz[0];
        (void)n;
        return o;
    }
    // Materialise modifiers (neg/abs) so the value can feed integer and bitwise instructions.
    Val Plain(Val v) {
        if (!v.o.neg && !v.o.abs)
            return v;
        if (v.o.k == Opnd::Imm) {
            for (int c = 0; c < 4; c++) {
                float f;
                std::memcpy(&f, &v.o.imm[c], 4);
                if (v.o.abs)
                    f = std::fabs(f);
                if (v.o.neg)
                    f = -f;
                std::memcpy(&v.o.imm[c], &f, 4);
            }
            v.o.neg = v.o.abs = false;
            return v;
        }
        return Op(OP_MOV, v.t, {v});
    }

    // ---------------- conversions
    Val Convert(Val v, Ty to) {
        if (to.b == B::Void)
            return v;
        if (v.t.n != to.n) {
            if (v.t.n == 1) {
                v.t.n = to.n; // broadcast
                for (int k = 1; k < 4; k++)
                    v.o.swz[k] = v.o.swz[0];
            } else if (v.t.n > to.n) {
                v.t.n = to.n; // truncate
            } else {
                Fail("cannot widen vector");
            }
        }
        if (v.t.b == to.b)
            return v;
        const Ty t{to.b, to.n};
        auto is_int = [](B b) { return b == B::Int || b == B::Uint; };
        if (is_int(v.t.b) && is_int(to.b)) {
            v.t.b = to.b;
            return v;
        }
        if (v.o.k == Opnd::Imm && !v.o.neg && !v.o.abs) {
            Val r = v;
            r.t = t;
            for (int c = 0; c < 4; c++) {
                const u32 x = v.o.imm[c];
                float f;
                std::memcpy(&f, &x, 4);
                u32 y = x;
                if (to.b == B::Bool)
                    y = v.t.b == B::Float ? (f != 0.0f ? ~0u : 0u) : (x ? ~0u : 0u);
                else if (to.b == B::Float) {
                    const float g = v.t.b == B::Int ? float(s32(x)) : v.t.b == B::Uint ? float(x) : (x ? 1.0f : 0.0f);
                    std::memcpy(&y, &g, 4);
                } else if (v.t.b == B::Float)
                    y = to.b == B::Int ? u32(s32(f)) : u32(f < 0 ? 0 : f);
                else if (v.t.b == B::Bool)
                    y = x ? 1u : 0u;
                r.o.imm[c] = y;
            }
            return r;
        }
        switch (to.b) {
        case B::Bool:
            if (v.t.b == B::Float)
                return Op(OP_NE, t, {v, Imm({B::Float, 1}, 0)});
            return Op(OP_INE, t, {Plain(v), Imm({B::Int, 1}, 0)});
        case B::Float:
            if (v.t.b == B::Bool)
                return Op(OP_AND, t, {v, Imm({B::Uint, 1}, 0x3F800000u)});
            return Op(v.t.b == B::Int ? OP_ITOF : OP_UTOF, t, {Plain(v)});
        case B::Int:
        case B::Uint:
            if (v.t.b == B::Bool)
                return Op(OP_AND, t, {v, Imm({B::Uint, 1}, 1)});
            return Op(to.b == B::Int ? OP_FTOI : OP_FTOU, t, {v});
        default:
            Fail("bad conversion");
        }
    }
    static B Promote(B a, B b) {
        if (a == B::Float || b == B::Float)
            return B::Float;
        if (a == B::Uint || b == B::Uint)
            return B::Uint;
        return B::Int;
    }

    // ---------------- variables
    const VarLoc* Lookup(const std::string& n) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
            if (auto f = it->find(n); f != it->end())
                return &f->second;
        if (auto g = globals_.find(n); g != globals_.end())
            return &g->second;
        return nullptr;
    }
    static Val Read(const VarLoc& v) {
        if (v.k == VarLoc::Const)
            return v.constant;
        Val r;
        r.t = v.t;
        if (v.k == VarLoc::Input) {
            r.o.k = Opnd::Input;
            r.o.reg = v.reg;
            return r;
        }
        if (v.k == VarLoc::InReg) {
            r.o.k = Opnd::InReg;
            r.o.reg = v.reg;
            return r; // (scalar members are in .x)
        }
        r.o.k = Opnd::Temp;
        r.o.reg = v.reg;
        if (v.t.n == 1)
            for (int k = 0; k < 4; k++)
                r.o.swz[k] = v.comp;
        return r;
    }
    // Writes value (already of the variable's type) to components `comps` of variable v.
    void Store(const VarLoc& v, Val val, const std::vector<u8>* comps) {
        if (v.k != VarLoc::Temp)
            Fail("assignment to non-temp variable");
        std::vector<u8> cs;
        if (comps)
            cs = *comps;
        else
            for (u8 c = 0; c < v.t.n; c++)
                cs.push_back(c);
        u8 mask = 0;
        Opnd src = val.o;
        u8 swz[4] = {0, 0, 0, 0};
        for (size_t i = 0; i < cs.size(); i++) {
            const u8 dc = v.t.n == 1 ? v.comp : u8(cs[i]);
            mask |= u8(1u << dc);
            swz[dc] = val.o.swz[val.t.n == 1 ? 0 : i];
        }
        std::memcpy(src.swz, swz, 4);
        std::vector<u32> tok;
        PutDst(tok, {OT_TEMP, v.reg, mask});
        PutSrc(tok, src, false);
        Emit(OP_MOV, tok);
    }

    // ---------------- expressions
    struct LValue {
        enum K { Var, ITemp, Tgsm } k = Var;
        const VarLoc* var = nullptr;
        std::vector<u8> comps; // Var: written components (empty = whole)
        Val index;             // ITemp/Tgsm element index
        Ty t;
    };
    static bool IsSwizzle(const std::string& m) {
        if (m.empty() || m.size() > 4)
            return false;
        for (char c : m)
            if (!std::strchr("xyzwrgba", c))
                return false;
        return true;
    }
    static u8 CompOf(char c) {
        switch (c) {
        case 'x': case 'r': return 0;
        case 'y': case 'g': return 1;
        case 'z': case 'b': return 2;
        default: return 3;
        }
    }
    // `s.m` where s is a struct variable: the member's own variable (registered as "s.m"), else null.
    const VarLoc* MemberVar(const Expr* e) {
        if (e->k != Expr::Member || e->a[0]->k != Expr::Name)
            return nullptr;
        return Lookup(e->a[0]->op + "." + e->op);
    }
    LValue EvalLValue(const Expr* e) {
        LValue lv;
        if (const VarLoc* mv = MemberVar(e)) {
            lv.var = mv;
            lv.t = mv->t;
            return lv;
        }
        if (e->k == Expr::Name) {
            const VarLoc* v = Lookup(e->op);
            if (!v)
                Fail("unknown variable " + e->op);
            lv.var = v;
            lv.t = v->t;
            return lv;
        }
        if (e->k == Expr::Member && IsSwizzle(e->op)) {
            LValue base = EvalLValue(e->a[0].get());
            if (base.k != LValue::Var || !base.comps.empty())
                Fail("unsupported swizzle target");
            for (char c : e->op)
                base.comps.push_back(CompOf(c));
            base.t.n = u8(e->op.size());
            return base;
        }
        if (e->k == Expr::Index && e->a[0]->k == Expr::Name) {
            const VarLoc* v = Lookup(e->a[0]->op);
            if (!v)
                Fail("unknown array " + e->a[0]->op);
            if (v->k == VarLoc::ITemp || v->k == VarLoc::Tgsm) {
                lv.k = v->k == VarLoc::ITemp ? LValue::ITemp : LValue::Tgsm;
                lv.var = v;
                lv.t = v->t;
                lv.index = Convert(Eval(e->a[1].get()), {B::Uint, 1});
                return lv;
            }
            if (v->k == VarLoc::Temp && v->t.n > 1) { // vector element with a constant index
                Val i = Eval(e->a[1].get());
                if (i.o.k != Opnd::Imm)
                    Fail("dynamic vector element write");
                lv.var = v;
                lv.comps = {u8(i.o.imm[i.o.swz[0]])};
                lv.t = {v->t.b, 1};
                return lv;
            }
        }
        Fail("unsupported assignment target");
    }
    void WriteLValue(const LValue& lv, Val v) {
        v = Convert(v, lv.t);
        if (lv.k == LValue::Var) {
            Store(*lv.var, Plain(v), lv.comps.empty() ? nullptr : &lv.comps);
            return;
        }
        Val idx = ToScalarTemp(lv.index);
        v = Plain(v);
        if (lv.k == LValue::ITemp) {
            std::vector<u32> tok;
            Dst d{OT_INDEXABLE_TEMP, lv.var->reg, 1};
            if (idx.o.k == Opnd::Imm)
                d.idx = idx.o.imm[idx.o.swz[0]];
            else
                d.rel = int(idx.o.reg), d.rel_comp = idx.o.swz[0];
            PutDst(tok, d);
            PutSrc(tok, v.o, true);
            Emit(OP_MOV, tok);
            return;
        }
        // groupshared: store_structured g#.x, index, 0, value
        std::vector<u32> tok;
        PutDst(tok, {OT_TGSM, lv.var->reg, 1});
        PutSrc(tok, idx.o, true);
        PutSrc(tok, Imm({B::Uint, 1}, 0).o, true);
        PutSrc(tok, v.o, true);
        Emit(OP_STORE_STRUCTURED, tok);
    }
    Val ToScalarTemp(Val v) {
        if (v.o.k == Opnd::Imm || (v.o.k == Opnd::Temp && !v.o.neg && !v.o.abs))
            return v;
        return Op(OP_MOV, {v.t.b, 1}, {v});
    }
    Val ReadLValue(const LValue& lv) {
        if (lv.k == LValue::Var) {
            Val v = Read(*lv.var);
            if (!lv.comps.empty())
                v = Swizzle(v, lv.comps.data(), int(lv.comps.size()));
            return v;
        }
        Val idx = ToScalarTemp(lv.index);
        if (lv.k == LValue::ITemp) {
            Val r;
            r.t = lv.t;
            r.o.k = Opnd::ITemp;
            r.o.reg = lv.var->reg;
            if (idx.o.k == Opnd::Imm)
                r.o.idx = idx.o.imm[idx.o.swz[0]];
            else
                r.o.rel = int(idx.o.reg), r.o.rel_comp = idx.o.swz[0];
            for (int k = 0; k < 4; k++)
                r.o.swz[k] = 0;
            return Op(OP_MOV, lv.t, {r});
        }
        const u32 out = Scratch();
        std::vector<u32> tok;
        PutDst(tok, {OT_TEMP, out, 1});
        PutSrc(tok, idx.o, true);
        PutSrc(tok, Imm({B::Uint, 1}, 0).o, true);
            tok.push_back(2u | (2u << 2) | (OT_TGSM << 12) | (1u << 20)); // g#.x (select_1)
        tok.push_back(lv.var->reg);
        Emit(OP_LD_STRUCTURED, tok);
        return TempVal(lv.t, out);
    }

    Val EvalAssign(const Expr* e) {
        LValue lv = EvalLValue(e->a[0].get());
        Val rhs = Eval(e->a[1].get());
        if (e->op != "=") {
            const std::string bop = e->op.substr(0, e->op.size() - 1);
            rhs = Binary(bop, ReadLValue(lv), rhs);
        }
        rhs = Plain(Convert(rhs, lv.t));
        WriteLValue(lv, rhs);
        return rhs; // the assignment's value: no need to read the target back
    }

    Val Binary(const std::string& op, Val a, Val b) {
        if (op == "&&" || op == "||") {
            a = Convert(a, {B::Bool, std::max(a.t.n, b.t.n)});
            b = Convert(b, {B::Bool, a.t.n});
            return Op(op == "&&" ? OP_AND : OP_OR, a.t, {a, b});
        }
        const u8 n = std::max(a.t.n, b.t.n);
        const bool cmp = op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=";
        if (op == "<<" || op == ">>") {
            B tb = a.t.b == B::Bool ? B::Int : a.t.b;
            a = Plain(Convert(a, {tb, n}));
            b = Plain(Convert(b, {B::Uint, n}));
            return Op(op == "<<" ? OP_ISHL : tb == B::Int ? OP_ISHR : OP_USHR, {tb, n}, {a, b});
        }
        if (op == "&" || op == "|" || op == "^") {
            B tb = (a.t.b == B::Bool && b.t.b == B::Bool) ? B::Bool : Promote(a.t.b, b.t.b);
            if (tb == B::Float)
                Fail("bitwise op on float");
            a = Plain(Convert(a, {tb, n}));
            b = Plain(Convert(b, {tb, n}));
            return Op(op == "&" ? OP_AND : op == "|" ? OP_OR : OP_XOR, {tb, n}, {a, b});
        }
        const B tb = Promote(a.t.b == B::Bool ? B::Int : a.t.b, b.t.b == B::Bool ? B::Int : b.t.b);
        a = Convert(a, {tb, n});
        b = Convert(b, {tb, n});
        const Ty rt{cmp ? B::Bool : tb, n};
        if (tb == B::Float) {
            a = a.o.k == Opnd::Imm ? Plain(a) : a;
            b = b.o.k == Opnd::Imm ? Plain(b) : b;
            if (op == "+") return Op(OP_ADD, rt, {a, b});
            if (op == "-") {
                if (b.o.k == Opnd::Imm)
                    for (int c = 0; c < 4; c++)
                        b.o.imm[c] ^= 0x80000000u;
                else
                    b.o.neg = !b.o.neg;
                return Op(OP_ADD, rt, {a, b});
            }
            if (op == "*") return Op(OP_MUL, rt, {a, b});
            if (op == "/") return Op(OP_DIV, rt, {a, b});
            if (op == "==") return Op(OP_EQ, rt, {a, b});
            if (op == "!=") return Op(OP_NE, rt, {a, b});
            if (op == "<") return Op(OP_LT, rt, {a, b});
            if (op == ">") return Op(OP_LT, rt, {b, a});
            if (op == ">=") return Op(OP_GE, rt, {a, b});
            if (op == "<=") return Op(OP_GE, rt, {b, a});
            Fail("float operator " + op);
        }
        a = Plain(a);
        b = Plain(b);
        const bool s = tb == B::Int;
        if (op == "+") return Op(OP_IADD, rt, {a, b});
        if (op == "-") {
            if (b.o.k == Opnd::Imm)
                for (int c = 0; c < 4; c++)
                    b.o.imm[c] = u32(-s32(b.o.imm[c]));
            else
                b.o.neg = true;
            return Op(OP_IADD, rt, {a, b});
        }
        if (op == "*") {
            const u32 r = Scratch();
            std::vector<u32> tok;
            PutDst(tok, {OT_NULL});
            PutDst(tok, {OT_TEMP, r, u8((1u << n) - 1)});
            PutSrc(tok, Spread(a, n), n == 1);
            PutSrc(tok, Spread(b, n), n == 1);
            Emit(s ? OP_IMUL : OP_UMUL, tok);
            return TempVal(rt, r);
        }
        if (op == "/" || op == "%") {
            if (s)
                Fail("signed integer division");
            const u32 r = Scratch();
            std::vector<u32> tok;
            if (op == "/") {
                PutDst(tok, {OT_TEMP, r, u8((1u << n) - 1)});
                PutDst(tok, {OT_NULL});
            } else {
                PutDst(tok, {OT_NULL});
                PutDst(tok, {OT_TEMP, r, u8((1u << n) - 1)});
            }
            PutSrc(tok, Spread(a, n), n == 1);
            PutSrc(tok, Spread(b, n), n == 1);
            Emit(OP_UDIV, tok);
            return TempVal(rt, r);
        }
        if (op == "==") return Op(OP_IEQ, rt, {a, b});
        if (op == "!=") return Op(OP_INE, rt, {a, b});
        if (op == "<") return Op(s ? OP_ILT : OP_ULT, rt, {a, b});
        if (op == ">") return Op(s ? OP_ILT : OP_ULT, rt, {b, a});
        if (op == ">=") return Op(s ? OP_IGE : OP_UGE, rt, {a, b});
        if (op == "<=") return Op(s ? OP_IGE : OP_UGE, rt, {b, a});
        Fail("integer operator " + op);
    }

    Val Eval(const Expr* e) {
        switch (e->k) {
        case Expr::Lit:
            return Imm(e->lit_ty, e->lit);
        case Expr::Name: {
            const VarLoc* v = Lookup(e->op);
            if (v) {
                if (v->k == VarLoc::ITemp || v->k == VarLoc::Tgsm)
                    Fail("array used as value: " + e->op);
                return Read(*v);
            }
            if (auto f = p_.cbfields.find(e->op); f != p_.cbfields.end() && !f->second.array) {
                Val r;
                r.t = f->second.t;
                r.o.k = Opnd::CB;
                r.o.reg = f->second.slot;
                r.o.idx = f->second.row;
                return r;
            }
            Fail("unknown name " + e->op);
        }
        case Expr::Unary: {
            if (e->op == "-" && e->a[0]->k == Expr::Lit) { // fold literal negation (keeps -0.0f)
                Val v = Eval(e->a[0].get());
                for (int c = 0; c < 4; c++)
                    v.o.imm[c] = v.t.b == B::Float ? v.o.imm[c] ^ 0x80000000u : u32(-s32(v.o.imm[c]));
                return v;
            }
            Val v = Eval(e->a[0].get());
            if (e->op == "+")
                return v;
            if (e->op == "-") {
                if (v.t.b == B::Float) {
                    if (v.o.k == Opnd::Imm) {
                        v = Plain(v);
                        for (int c = 0; c < 4; c++)
                            v.o.imm[c] ^= 0x80000000u;
                    } else {
                        v.o.neg = !v.o.neg;
                    }
                    return v;
                }
                v = Plain(Convert(v, {v.t.b == B::Bool ? B::Int : v.t.b, v.t.n}));
                return Op(OP_INEG, v.t, {v});
            }
            if (e->op == "!") {
                // ieq x, 0 rather than not: drivers mis-optimise not(not(bool)) chains, which FXC never emits.
                v = Convert(v, {B::Bool, v.t.n});
                return Op(OP_IEQ, v.t, {v, Imm({B::Uint, 1}, 0)});
            }
            if (e->op == "~") {
                v = Plain(v);
                return Op(OP_NOT, v.t, {v});
            }
            Fail("unary " + e->op);
        }
        case Expr::Binary:
            return Binary(e->op, Eval(e->a[0].get()), Eval(e->a[1].get()));
        case Expr::Cond: {
            Val c = Eval(e->a[0].get());
            Val x = Eval(e->a[1].get()), y = Eval(e->a[2].get());
            const u8 n = std::max(x.t.n, y.t.n);
            B tb = x.t.b == y.t.b ? x.t.b : Promote(x.t.b, y.t.b);
            x = Plain(Convert(x, {tb, n}));
            y = Plain(Convert(y, {tb, n}));
            c = Convert(c, {B::Bool, n});
            return Op(OP_MOVC, {tb, n}, {c, x, y});
        }
        case Expr::Assign:
            return EvalAssign(e);
        case Expr::IncDec: {
            LValue lv = EvalLValue(e->a[0].get());
            Val old = ReadLValue(lv);
            if (e->post)
                old = Op(OP_MOV, old.t, {old});
            Val one = old.t.b == B::Float ? ImmF(1.0f) : Imm({old.t.b, 1}, 1);
            Val nv = Binary(e->op == "++" ? "+" : "-", ReadLValue(lv), one);
            WriteLValue(lv, nv);
            return e->post ? old : ReadLValue(lv);
        }
        case Expr::Member: {
            if (const VarLoc* mv = MemberVar(e))
                return Read(*mv);
            if (!IsSwizzle(e->op))
                Fail("member " + e->op);
            Val v = Eval(e->a[0].get());
            u8 comps[4];
            for (size_t i = 0; i < e->op.size(); i++)
                comps[i] = CompOf(e->op[i]);
            return Swizzle(v, comps, int(e->op.size()));
        }
        case Expr::Index:
            return EvalIndex(e);
        case Expr::Call:
            return EvalCall(e);
        case Expr::Method:
            return EvalMethod(e);
        }
        Fail("expression");
    }

    Val EvalIndex(const Expr* e) {
        if (e->a[0]->k == Expr::Name) {
            const std::string& name = e->a[0]->op;
            if (auto f = p_.cbfields.find(name); f != p_.cbfields.end()) {
                Val i = Convert(Eval(e->a[1].get()), {B::Uint, 1});
                Val r;
                r.t = f->second.t;
                r.o.k = Opnd::CB;
                r.o.reg = f->second.slot;
                r.o.idx = f->second.row;
                if (i.o.k == Opnd::Imm) {
                    r.o.idx += i.o.imm[i.o.swz[0]];
                } else {
                    i = ToScalarTemp(Plain(i));
                    r.o.rel = int(i.o.reg);
                    r.o.rel_comp = i.o.swz[0];
                    cb_dynamic_[f->second.slot] = true;
                }
                return r;
            }
            const VarLoc* v = Lookup(name);
            if (v && (v->k == VarLoc::ITemp || v->k == VarLoc::Tgsm))
                return ReadLValue(EvalLValue(e));
        }
        Val base = Eval(e->a[0].get());
        Val i = Eval(e->a[1].get());
        if (i.o.k != Opnd::Imm || base.t.n == 1)
            Fail("dynamic vector indexing");
        const u8 c = u8(i.o.imm[i.o.swz[0]]);
        return Swizzle(base, &c, 1);
    }

    // ---------------- calls
    Val Arg(const Expr* e, size_t i) { return Eval(e->a[i].get()); }
    Val EvalCall(const Expr* e) {
        const std::string& n = e->op;
        Ty ct;
        if (ParseScalarType(n, ct)) { // constructors / casts
            if (e->a.size() == 1) {
                Val v = Eval(e->a[0].get());
                if (ct.n > 1 && v.t.n == 1)
                    return Convert(v, ct);
                return Convert(v, ct);
            }
            const u32 r = Scratch();
            u8 comp = 0;
            for (auto& a : e->a) {
                Val v = Eval(a.get());
                v = Plain(Convert(v, {ct.b, v.t.n}));
                for (u8 k = 0; k < v.t.n; k++, comp++) {
                    std::vector<u32> tok;
                    PutDst(tok, {OT_TEMP, r, u8(1u << comp)});
                    PutSrc(tok, Comp(v, k), false);
                    Emit(OP_MOV, tok);
                }
            }
            if (comp != ct.n)
                Fail("constructor component count");
            return TempVal(ct, r);
        }
        if (n == "asfloat" || n == "asuint" || n == "asint") {
            Val v = Eval(e->a[0].get());
            const B nb = n == "asfloat" ? B::Float : n == "asuint" ? B::Uint : B::Int;
            if (nb != B::Float || v.t.b != B::Float)
                v = Plain(v);
            v.t.b = nb;
            return v;
        }
        if (n == "saturate") {
            Val v = Arg(e, 0);
            v = Convert(v, {B::Float, v.t.n});
            return Op(OP_MOV, v.t, {v}, kSaturate);
        }
        auto f1 = [&](u32 op) {
            Val v = Eval(e->a[0].get());
            v = Convert(v, {B::Float, v.t.n});
            return Op(op, v.t, {v});
        };
        if (n == "floor") return f1(OP_ROUND_NI);
        if (n == "ceil") return f1(OP_ROUND_PI);
        if (n == "trunc") return f1(OP_ROUND_Z);
        if (n == "round") return f1(OP_ROUND_NE);
        if (n == "frac") return f1(OP_FRC);
        if (n == "sqrt") return f1(OP_SQRT);
        if (n == "rsqrt") return f1(OP_RSQ);
        if (n == "exp2") return f1(OP_EXP);
        if (n == "log2") return f1(OP_LOG);
        if (n == "rcp") { Val v = Eval(e->a[0].get()); v = Convert(v, {B::Float, v.t.n}); return Op(OP_DIV, v.t, {ImmF(1.0f), v}); }
        if (n == "sin" || n == "cos") {
            Val v = Eval(e->a[0].get());
            v = Convert(v, {B::Float, v.t.n});
            const u32 r = Scratch();
            std::vector<u32> tok;
            const u8 m = u8((1u << v.t.n) - 1);
            if (n == "sin") {
                PutDst(tok, {OT_TEMP, r, m});
                PutDst(tok, {OT_NULL});
            } else {
                PutDst(tok, {OT_NULL});
                PutDst(tok, {OT_TEMP, r, m});
            }
            PutSrc(tok, Spread(v, v.t.n), v.t.n == 1);
            Emit(OP_SINCOS, tok);
            return TempVal(v.t, r);
        }
        if (n == "abs") {
            Val v = Eval(e->a[0].get());
            if (v.t.b == B::Float) {
                v.o.abs = true;
                v.o.neg = false;
                return v.o.k == Opnd::Imm ? Plain(v) : v;
            }
            v = Plain(v);
            Val neg = Op(OP_INEG, v.t, {v});
            return Op(OP_IMAX, v.t, {v, neg});
        }
        if (n == "min" || n == "max") {
            Val a = Arg(e, 0), b = Arg(e, 1);
            const B tb = Promote(a.t.b, b.t.b);
            const u8 k = std::max(a.t.n, b.t.n);
            a = Convert(a, {tb, k});
            b = Convert(b, {tb, k});
            if (tb != B::Float)
                a = Plain(a), b = Plain(b);
            const u32 op = tb == B::Float ? (n == "min" ? OP_MIN : OP_MAX)
                         : tb == B::Int ? (n == "min" ? OP_IMIN : OP_IMAX) : (n == "min" ? OP_UMIN : OP_UMAX);
            return Op(op, {tb, k}, {a, b});
        }
        if (n == "clamp") {
            Val x = Arg(e, 0), lo = Arg(e, 1), hi = Arg(e, 2);
            const B tb = Promote(Promote(x.t.b, lo.t.b), hi.t.b);
            const u8 k = std::max({x.t.n, lo.t.n, hi.t.n});
            x = Convert(x, {tb, k});
            lo = Convert(lo, {tb, k});
            hi = Convert(hi, {tb, k});
            if (tb != B::Float)
                x = Plain(x), lo = Plain(lo), hi = Plain(hi);
            const u32 mx = tb == B::Float ? OP_MAX : tb == B::Int ? OP_IMAX : OP_UMAX;
            const u32 mn = tb == B::Float ? OP_MIN : tb == B::Int ? OP_IMIN : OP_UMIN;
            Val t = Op(mx, {tb, k}, {x, lo});
            return Op(mn, {tb, k}, {t, hi});
        }
        if (n == "mad") {
            Val a = Arg(e, 0), b = Arg(e, 1), c = Arg(e, 2);
            const B tb = Promote(Promote(a.t.b, b.t.b), c.t.b);
            const u8 k = std::max({a.t.n, b.t.n, c.t.n});
            a = Convert(a, {tb, k});
            b = Convert(b, {tb, k});
            c = Convert(c, {tb, k});
            if (tb != B::Float)
                a = Plain(a), b = Plain(b), c = Plain(c);
            return Op(tb == B::Float ? OP_MAD : tb == B::Int ? OP_IMAD : OP_UMAD, {tb, k}, {a, b, c});
        }
        if (n == "countbits") {
            Val v = Plain(Arg(e, 0));
            return Op(OP_COUNTBITS, {B::Uint, v.t.n}, {v});
        }
        if (n == "firstbithigh") { // DXBC counts from the MSB; HLSL returns the bit index, or ~0 for zero
            Val v = Plain(Arg(e, 0));
            Val f = Op(OP_FIRSTBIT_HI, {B::Uint, v.t.n}, {v});
            f.o.neg = true;
            Val idx = Op(OP_IADD, {B::Uint, v.t.n}, {f, Imm({B::Uint, 1}, 31)});
            return Op(OP_MOVC, {B::Uint, v.t.n}, {v, idx, Imm({B::Uint, 1}, ~0u)});
        }
        if (n == "firstbitlow") {
            Val v = Plain(Arg(e, 0));
            return Op(OP_FIRSTBIT_LO, {B::Uint, v.t.n}, {v});
        }
        if (n == "reversebits") {
            Val v = Plain(Arg(e, 0));
            return Op(OP_BFREV, {B::Uint, v.t.n}, {v});
        }
        if (n == "f32tof16") {
            Val v = Arg(e, 0);
            v = Convert(v, {B::Float, v.t.n});
            return Op(OP_F32TOF16, {B::Uint, v.t.n}, {v});
        }
        if (n == "f16tof32") {
            Val v = Plain(Arg(e, 0));
            return Op(OP_F16TOF32, {B::Float, v.t.n}, {v});
        }
        if (n == "umul" || n == "imul") { // umul(out hi, out lo, a, b)
            LValue hi = EvalLValue(e->a[0].get()), lo = EvalLValue(e->a[1].get());
            Val a = Plain(Arg(e, 2)), b = Plain(Arg(e, 3));
            const u32 rh = Scratch(), rl = Scratch();
            std::vector<u32> tok;
            PutDst(tok, {OT_TEMP, rh, 1});
            PutDst(tok, {OT_TEMP, rl, 1});
            PutSrc(tok, a.o, true);
            PutSrc(tok, b.o, true);
            Emit(n == "umul" ? OP_UMUL : OP_IMUL, tok);
            WriteLValue(hi, TempVal({B::Uint, 1}, rh));
            WriteLValue(lo, TempVal({B::Uint, 1}, rl));
            return Imm({B::Void, 1}, 0);
        }
        if (n == "GroupMemoryBarrierWithGroupSync") {
            Emit(OP_SYNC, {}, (1u << 11) | (2u << 11));
            return Imm({B::Void, 1}, 0);
        }
        if (n == "GroupMemoryBarrier") {
            Emit(OP_SYNC, {}, 2u << 11);
            return Imm({B::Void, 1}, 0);
        }
        if (n == "AllMemoryBarrierWithGroupSync" || n == "DeviceMemoryBarrierWithGroupSync") {
            Emit(OP_SYNC, {}, (1u << 11) | (2u << 11) | (8u << 11));
            return Imm({B::Void, 1}, 0);
        }
        if (n == "AllMemoryBarrier" || n == "DeviceMemoryBarrier") {
            Emit(OP_SYNC, {}, (2u << 11) | (8u << 11));
            return Imm({B::Void, 1}, 0);
        }
        if (n.rfind("Interlocked", 0) == 0) { // on groupshared: Interlocked*(smem[i], v, out orig)
            LValue lv = EvalLValue(e->a[0].get());
            if (lv.k != LValue::Tgsm)
                Fail(n + " target");
            Val v = Plain(Convert(Arg(e, 1), {B::Uint, 1}));
            Val idx = ToScalarTemp(lv.index);
            // structured address: (index, byte offset 0)
            const u32 ar = Scratch();
            {
                std::vector<u32> tok;
                PutDst(tok, {OT_TEMP, ar, 1});
                PutSrc(tok, idx.o, true);
                Emit(OP_MOV, tok);
                tok.clear();
                PutDst(tok, {OT_TEMP, ar, 2});
                PutSrc(tok, Imm({B::Uint, 1}, 0).o, true);
                Emit(OP_MOV, tok);
            }
            Opnd addr;
            addr.k = Opnd::Temp;
            addr.reg = ar;
            const u8 sw[4] = {0, 1, 0, 0};
            std::memcpy(addr.swz, sw, 4);
            const u32 op = AtomicOp(n, e->a.size() == 3);
            std::vector<u32> tok;
            u32 out = 0;
            if (e->a.size() == 3) {
                out = Scratch();
                PutDst(tok, {OT_TEMP, out, 1});
            }
            tok.push_back(OT_TGSM << 12 | (1u << 20));
            tok.push_back(lv.var->reg);
            PutSrc(tok, addr, false);
            PutSrc(tok, v.o, true);
            Emit(op, tok);
            if (e->a.size() == 3)
                WriteLValue(EvalLValue(e->a[2].get()), TempVal({B::Uint, 1}, out));
            return Imm({B::Void, 1}, 0);
        }
        // user function
        auto fit = p_.funcs.find(n);
        if (fit == p_.funcs.end())
            Fail("unknown function " + n);
        return CallUser(e, fit->second);
    }
    static u32 AtomicOp(const std::string& n, bool with_result) {
        const std::string k = n.substr(11);
        if (with_result) {
            if (k == "Add") return OP_IMM_ATOMIC_IADD;
            if (k == "And") return OP_IMM_ATOMIC_AND;
            if (k == "Or") return OP_IMM_ATOMIC_OR;
            if (k == "Xor") return OP_IMM_ATOMIC_XOR;
            if (k == "Exchange") return OP_IMM_ATOMIC_EXCH;
            if (k == "Min") return OP_IMM_ATOMIC_UMIN;
            if (k == "Max") return OP_IMM_ATOMIC_UMAX;
        } else {
            if (k == "Add") return OP_ATOMIC_IADD;
            if (k == "And") return OP_ATOMIC_AND;
            if (k == "Or") return OP_ATOMIC_OR;
            if (k == "Xor") return OP_ATOMIC_XOR;
            if (k == "Min") return OP_ATOMIC_UMIN;
            if (k == "Max") return OP_ATOMIC_UMAX;
        }
        Fail("atomic " + n);
    }

    Val CallUser(const Expr* e, const Func& f) {
        FuncInfo& fi = fn_[f.name];
        if (!fi.f) {
            fi.f = &f;
            fi.label = next_label_++;
            // Parameters and the result live in the callee's own pool.
            Pool& pl = pools_.emplace_back();
            Pool* saved = cur_pool_;
            cur_pool_ = &pl;
            for (auto& prm : f.params) {
                VarLoc v;
                v.t = prm.t;
                AllocVar(v, false);
                fi.params.push_back(v);
            }
            if (f.ret.b != B::Void) {
                fi.ret.t = f.ret;
                AllocVar(fi.ret, false);
            }
            fi.pool = &pl;
            cur_pool_ = saved;
        }
        if (!fi.queued) {
            fi.queued = true;
            queue_.push_back(&fi);
        }
        if (e->a.size() != f.params.size())
            Fail("argument count for " + f.name);
        for (size_t i = 0; i < f.params.size(); i++)
            if (f.params[i].in)
                Store(fi.params[i], Plain(Convert(Eval(e->a[i].get()), f.params[i].t)), nullptr);
        std::vector<u32> tok;
        tok.push_back(OT_LABEL << 12 | (1u << 20));
        tok.push_back(fi.label);
        Emit(OP_CALL, tok);
        for (size_t i = 0; i < f.params.size(); i++)
            if (f.params[i].out)
                WriteLValue(EvalLValue(e->a[i].get()), Read(fi.params[i]));
        if (f.ret.b == B::Void)
            return Imm({B::Void, 1}, 0);
        Val r = Read(fi.ret);
        return Op(OP_MOV, r.t, {r}); // copy out: a later call overwrites the result register
    }

    void CompileFunc(FuncInfo& fi) {
        Pool* saved = cur_pool_;
        cur_pool_ = fi.pool;
        FuncInfo* saved_fn = cur_fn_;
        cur_fn_ = &fi;
        std::vector<u32> tok;
        tok.push_back(OT_LABEL << 12 | (1u << 20));
        tok.push_back(fi.label);
        Emit(OP_LABEL, tok);
        scopes_.emplace_back();
        for (size_t i = 0; i < fi.f->params.size(); i++)
            scopes_.back()[fi.f->params[i].name] = fi.params[i];
        Frame fr;
        fr.loop_base = loops_.size();
        fr.flag.t = {B::Bool, 1};
        AllocVar(fr.flag, false);
        Store(fr.flag, Imm({B::Bool, 1}, 0), nullptr);
        frames_.push_back(fr);
        Emit(OP_LOOP, {});
        CompileStmt(fi.f->body.get());
        Emit(OP_BREAK, {});
        Emit(OP_ENDLOOP, {});
        frames_.pop_back();
        scopes_.pop_back();
        Emit(OP_RET, {});
        cur_fn_ = saved_fn;
        cur_pool_ = saved;
    }

    // ---------------- resource methods
    const Resource& Res(const std::string& n) {
        auto it = p_.resources.find(n);
        if (it == p_.resources.end())
            Fail("unknown resource " + n);
        used_resources_.insert(n);
        return it->second;
    }
    static u32 ResDim(const std::string& d) {
        if (d == "Buffer") return 1;
        if (d == "Texture1D") return 2;
        if (d == "Texture2D") return 3;
        if (d == "Texture3D") return 5;
        if (d == "TextureCube") return 6;
        if (d == "Texture1DArray") return 7;
        if (d == "Texture2DArray") return 8;
        if (d == "TextureCubeArray") return 10;
        Fail("resource dimension " + d);
    }
    static u32 RetType(B b) { return b == B::Float ? 5u : b == B::Int ? 3u : 4u; }
    void PutResource(std::vector<u32>& t, u32 type, u32 reg, bool scalar_x = false) {
        if (scalar_x)
            t.push_back(2u | (2u << 2) | (type << 12) | (1u << 20)); // .x select
        else
            t.push_back(2u | (1u << 2) | (0xE4u << 4) | (type << 12) | (1u << 20)); // .xyzw
        t.push_back(reg);
    }
    // Packs `vals` (ints) into one temp's components: returns a 4-component value (unused components 0).
    Val Pack(std::initializer_list<Val> vals, B b) {
        const u32 r = Scratch();
        u8 comp = 0;
        for (Val v : vals) {
            v = Plain(Convert(v, {b, v.t.n}));
            for (u8 k = 0; k < v.t.n; k++, comp++) {
                std::vector<u32> tok;
                PutDst(tok, {OT_TEMP, r, u8(1u << comp)});
                PutSrc(tok, Comp(v, k), false);
                Emit(OP_MOV, tok);
            }
        }
        for (; comp < 4; comp++) {
            std::vector<u32> tok;
            PutDst(tok, {OT_TEMP, r, u8(1u << comp)});
            PutSrc(tok, Imm({b, 1}, 0).o, false);
            Emit(OP_MOV, tok);
        }
        return TempVal({b, 4}, r);
    }
    Val EvalMethod(const Expr* e) {
        if (e->a[0]->k != Expr::Name)
            Fail("method on expression");
        const std::string& obj = e->a[0]->op;
        const Resource& r = Res(obj);
        const std::string& m = e->op;
        auto arg = [&](size_t i) { return Eval(e->a[i + 1].get()); };
        if (r.kind == ResKind::RawBuffer || r.kind == ResKind::RWRawBuffer) {
            const u32 type = r.kind == ResKind::RawBuffer ? OT_RESOURCE : OT_UAV;
            if (m == "Load") {
                Val a = Plain(Convert(arg(0), {B::Uint, 1}));
                const u32 out = Scratch();
                std::vector<u32> tok;
                PutDst(tok, {OT_TEMP, out, 1});
                PutSrc(tok, a.o, true);
                PutResource(tok, type, r.reg, true);
                Emit(OP_LD_RAW, tok);
                return TempVal({B::Uint, 1}, out);
            }
            if (m == "Store" && type == OT_UAV) {
                Val a = Plain(Convert(arg(0), {B::Uint, 1}));
                Val v = Plain(Convert(arg(1), {B::Uint, 1}));
                std::vector<u32> tok;
                tok.push_back(2u | (1u << 4) | (OT_UAV << 12) | (1u << 20)); // u#.x
                tok.push_back(r.reg);
                PutSrc(tok, a.o, true);
                PutSrc(tok, v.o, true);
                Emit(OP_STORE_RAW, tok);
                return Imm({B::Void, 1}, 0);
            }
            if (m.rfind("Interlocked", 0) == 0 && type == OT_UAV) {
                Val a = Plain(Convert(arg(0), {B::Uint, 1}));
                Val v = Plain(Convert(arg(1), {B::Uint, 1}));
                const bool res = e->a.size() == 4;
                std::vector<u32> tok;
                u32 out = 0;
                if (res) {
                    out = Scratch();
                    PutDst(tok, {OT_TEMP, out, 1});
                }
                tok.push_back(OT_UAV << 12 | (1u << 20));
                tok.push_back(r.reg);
                PutSrc(tok, a.o, true);
                PutSrc(tok, v.o, true);
                Emit(AtomicOp(m, res), tok);
                if (res)
                    WriteLValue(EvalLValue(e->a[3].get()), TempVal({B::Uint, 1}, out));
                return Imm({B::Void, 1}, 0);
            }
            Fail("raw buffer method " + m);
        }
        if (r.kind != ResKind::Texture)
            Fail("method on sampler");
        const u32 dim = ResDim(r.dim);
        const std::vector<u32> ext = {0x80000002u | (dim << 6),
                                      3u | (RetType(r.elem.b) << 6) | (RetType(r.elem.b) << 10) |
                                          (RetType(r.elem.b) << 14) | (RetType(r.elem.b) << 18)};
        const Ty rt{r.elem.b, 4};
        if (m == "Load") {
            Val c = arg(0);
            Val addr;
            if (r.dim == "Buffer") {
                addr = Plain(Convert(c, {B::Int, 1}));
            } else {
                // mip level goes to .w, coordinates first
                c = Plain(Convert(c, {B::Int, c.t.n}));
                const int nc = c.t.n - 1;
                const u32 reg = Scratch();
                for (int k = 0; k < 4; k++) {
                    std::vector<u32> tok;
                    PutDst(tok, {OT_TEMP, reg, u8(1u << k)});
                    if (k < nc)
                        PutSrc(tok, Comp(c, k), false);
                    else if (k == 3)
                        PutSrc(tok, Comp(c, nc), false);
                    else
                        PutSrc(tok, Imm({B::Int, 1}, 0).o, false);
                    Emit(OP_MOV, tok);
                }
                addr = TempVal({B::Int, 4}, reg);
            }
            const u32 out = Scratch();
            std::vector<u32> tok;
            PutDst(tok, {OT_TEMP, out, 0xF});
            PutSrc(tok, Spread(addr, 4), false);
            PutResource(tok, OT_RESOURCE, r.reg);
            EmitRaw(OP_LD, 0, tok, ext);
            return TempVal(rt, out);
        }
        if (m == "SampleLevel") {
            if (e->a[1]->k != Expr::Name)
                Fail("sampler expression");
            const Resource& s = Res(e->a[1]->op);
            Val c = arg(1);
            c = Convert(c, {B::Float, c.t.n});
            Val lod = Convert(arg(2), {B::Float, 1});
            c = Plain(c);
            const u32 out = Scratch();
            std::vector<u32> tok;
            PutDst(tok, {OT_TEMP, out, 0xF});
            PutSrc(tok, Spread(c, 4), false);
            PutResource(tok, OT_RESOURCE, r.reg);
            tok.push_back(OT_SAMPLER << 12 | (1u << 20));
            tok.push_back(s.reg);
            PutSrc(tok, Plain(lod).o, true);
            EmitRaw(OP_SAMPLE_L, 0, tok, ext);
            return TempVal(rt, out);
        }
        if (m == "SampleCmpLevelZero") {
            const Resource& s = Res(e->a[1]->op);
            Val c = arg(1);
            c = Plain(Convert(c, {B::Float, c.t.n}));
            Val ref = Plain(Convert(arg(2), {B::Float, 1}));
            const u32 out = Scratch();
            std::vector<u32> tok;
            PutDst(tok, {OT_TEMP, out, 1});
            PutSrc(tok, Spread(c, 4), false);
            tok.push_back(2u | (2u << 2) | (OT_RESOURCE << 12) | (1u << 20)); // .x
            tok.push_back(r.reg);
            tok.push_back(OT_SAMPLER << 12 | (1u << 20));
            tok.push_back(s.reg);
            PutSrc(tok, ref.o, true);
            EmitRaw(OP_SAMPLE_C_LZ, 0, tok, ext);
            return TempVal({B::Float, 1}, out);
        }
        // Pixel-shader sampling (implicit derivatives) and gathers. The sampler operand of gather4 selects the
        // channel through its swizzle (s#.x red ... s#.w alpha).
        auto sampler = [&](std::vector<u32>& tok, int select) {
            if (e->a[1]->k != Expr::Name)
                Fail("sampler expression");
            const Resource& s = Res(e->a[1]->op);
            if (select >= 0)
                tok.push_back(2u | (2u << 2) | (u32(select) << 4) | (OT_SAMPLER << 12) | (1u << 20));
            else
                tok.push_back(OT_SAMPLER << 12 | (1u << 20));
            tok.push_back(s.reg);
        };
        auto coords = [&]() {
            Val c = arg(1);
            return Plain(Convert(c, {B::Float, c.t.n}));
        };
        if (m == "Sample" || m == "SampleBias") {
            Val c = coords();
            const u32 out = Scratch();
            std::vector<u32> tok;
            PutDst(tok, {OT_TEMP, out, 0xF});
            PutSrc(tok, Spread(c, 4), false);
            PutResource(tok, OT_RESOURCE, r.reg);
            sampler(tok, -1);
            if (m == "SampleBias")
                PutSrc(tok, Plain(Convert(arg(2), {B::Float, 1})).o, true);
            EmitRaw(m == "Sample" ? OP_SAMPLE : OP_SAMPLE_B, 0, tok, ext);
            return TempVal(rt, out);
        }
        if (m == "SampleCmp") {
            Val c = coords();
            Val ref = Plain(Convert(arg(2), {B::Float, 1}));
            const u32 out = Scratch();
            std::vector<u32> tok;
            PutDst(tok, {OT_TEMP, out, 1});
            PutSrc(tok, Spread(c, 4), false);
            tok.push_back(2u | (2u << 2) | (OT_RESOURCE << 12) | (1u << 20)); // .x
            tok.push_back(r.reg);
            sampler(tok, -1);
            PutSrc(tok, ref.o, true);
            EmitRaw(OP_SAMPLE_C, 0, tok, ext);
            return TempVal({B::Float, 1}, out);
        }
        if (m == "GatherRed" || m == "GatherGreen" || m == "GatherBlue" || m == "GatherAlpha" || m == "GatherCmp") {
            const bool cmp = m == "GatherCmp";
            Val c = coords();
            const u32 out = Scratch();
            std::vector<u32> tok;
            PutDst(tok, {OT_TEMP, out, 0xF});
            PutSrc(tok, Spread(c, 4), false);
            PutResource(tok, OT_RESOURCE, r.reg);
            sampler(tok, cmp ? 0 : m == "GatherRed" ? 0 : m == "GatherGreen" ? 1 : m == "GatherBlue" ? 2 : 3);
            if (cmp)
                PutSrc(tok, Plain(Convert(arg(2), {B::Float, 1})).o, true);
            EmitRaw(cmp ? OP_GATHER4_C : OP_GATHER4, 0, tok, ext);
            return TempVal(cmp ? Ty{B::Float, 4} : rt, out);
        }
        if (m == "CalculateLevelOfDetail") { // lod: x = clamped level (what this returns), y = unclamped
            Val c = coords();
            const u32 out = Scratch();
            std::vector<u32> tok;
            PutDst(tok, {OT_TEMP, out, 1});
            PutSrc(tok, Spread(c, 4), false);
            PutResource(tok, OT_RESOURCE, r.reg, true);
            sampler(tok, -1);
            EmitRaw(OP_LOD, 0, tok, ext);
            return TempVal({B::Float, 1}, out);
        }
        if (m == "GetDimensions" && r.dim != "Buffer") {
            // GetDimensions(mip, w[, h[, d]], levels) -> resinfo_uint: (w, h, d or slices, levels)
            const size_t nargs = e->a.size() - 1;
            if (nargs < 3)
                Fail("GetDimensions form");
            Val mip = Plain(Convert(arg(0), {B::Uint, 1}));
            const u32 out = Scratch();
            std::vector<u32> tok;
            PutDst(tok, {OT_TEMP, out, 0xF});
            PutSrc(tok, mip.o, true);
            PutResource(tok, OT_RESOURCE, r.reg);
            Emit(OP_RESINFO, tok, 2u << 11); // return type: uint
            Val res = TempVal({B::Uint, 4}, out);
            for (size_t i = 1; i < nargs; i++) {
                const u8 comp = i == nargs - 1 ? 3 : u8(i - 1);
                WriteLValue(EvalLValue(e->a[i + 1].get()), Swizzle(res, &comp, 1));
            }
            return Imm({B::Void, 1}, 0);
        }
        Fail("texture method " + m);
    }

    // ---------------- statements
    struct Loop {
        const Expr* inc = nullptr;
        bool is_switch = false;
        bool unrolled = false;
    };
    std::vector<Loop> loops_;
    // Subroutine frame: drivers end the whole thread on a `ret` nested in flow control, so a function body runs in
    // a one-iteration loop and returns become breaks (with a flag to leave nested loops/switches).
    struct Frame {
        size_t loop_base = 0;
        VarLoc flag;
    };
    std::vector<Frame> frames_;
    u32 NestedBreakables() const {
        u32 n = 0;
        for (size_t i = frames_.back().loop_base; i < loops_.size(); i++)
            n += loops_[i].unrolled ? 0 : 1;
        return n;
    }
    static bool ContainsReturn(const Stmt* s) {
        if (!s)
            return false;
        if (s->k == Stmt::Return)
            return true;
        for (auto& b : s->body)
            if (ContainsReturn(b.get()))
                return true;
        for (auto& c : s->cases)
            for (auto& b : c.second)
                if (ContainsReturn(b.get()))
                    return true;
        return false;
    }
    // After a loop or switch that may have returned: leave the next enclosing construct too.
    void PropagateReturn(const Stmt* construct) {
        if (frames_.empty() || !ContainsReturn(construct))
            return;
        std::vector<u32> tok;
        PutSrc(tok, Read(frames_.back().flag).o, true);
        Emit(OP_IF, tok, kTestNonZero);
        Emit(OP_BREAK, {});
        Emit(OP_ENDIF, {});
    }

    Val Cond(const Expr* e) {
        Val c = Convert(Eval(e), {B::Bool, 1});
        return ToScalarTemp(c);
    }
    void CompileStmt(const Stmt* s) {
        if (!s)
            return;
        switch (s->k) {
        case Stmt::Block:
            scopes_.emplace_back();
            for (auto& b : s->body)
                CompileStmt(b.get());
            scopes_.pop_back();
            return;
        case Stmt::ExprS:
            if (in_main_ && stage_ != Stage::Compute && s->e->k == Expr::Call && s->e->a.empty() &&
                s->e->op == "Run" && p_.funcs.count("Run") && p_.funcs.at("Run").ret.b == B::Void) {
                // The translated program (Run) inlined into main, in its own one-iteration loop so its returns
                // become breaks to the code after it (outputs): drivers compile a call much more slowly.
                scopes_.emplace_back();
                Frame fr;
                fr.loop_base = loops_.size();
                fr.flag.t = {B::Bool, 1};
                AllocVar(fr.flag, false);
                Store(fr.flag, Imm({B::Bool, 1}, 0), nullptr);
                frames_.push_back(fr);
                Emit(OP_LOOP, {});
                CompileStmt(p_.funcs.at("Run").body.get());
                Emit(OP_BREAK, {});
                Emit(OP_ENDLOOP, {});
                frames_.pop_back();
                scopes_.pop_back();
                EndStatement();
                return;
            }
            Eval(s->e.get());
            EndStatement();
            return;
        case Stmt::Decl:
            for (auto& d : s->decls) {
                if (!d.struct_name.empty()) { // the entry point's output struct: one temp per member
                    if (d.struct_name != main_ret_struct_ || !out_vars_.empty())
                        Fail("struct local " + d.name);
                    out_members_ = p_.structs.at(d.struct_name);
                    for (auto& m : out_members_) {
                        VarLoc v;
                        v.t = m.t;
                        AllocVar(v, false);
                        out_vars_.push_back(v);
                        scopes_.back()[d.name + "." + m.name] = v;
                    }
                    continue;
                }
                VarLoc v;
                v.t = d.t;
                if (d.array) {
                    v.k = VarLoc::ITemp;
                    v.reg = u32(itemps_.size());
                    v.array = d.array;
                    itemps_.push_back({d.array, d.t.n});
                } else {
                    AllocVar(v, false);
                }
                if (d.init && v.k == VarLoc::Temp) {
                    Store(v, Plain(Convert(Eval(d.init.get()), v.t)), nullptr);
                    EndStatement();
                }
                scopes_.back()[d.name] = v;
            }
            return;
        case Stmt::If: {
            Val c = Cond(s->e.get());
            std::vector<u32> tok;
            PutSrc(tok, c.o, true);
            EndStatement();
            Emit(OP_IF, tok, kTestNonZero);
            CompileStmt(s->body[0].get());
            if (s->body.size() > 1) {
                Emit(OP_ELSE, {});
                CompileStmt(s->body[1].get());
            }
            Emit(OP_ENDIF, {});
            return;
        }
        case Stmt::For:
            CompileFor(s);
            return;
        case Stmt::Switch: {
            Val v = ToScalarTemp(Plain(Convert(Eval(s->e.get()), {B::Uint, 1})));
            std::vector<u32> tok;
            PutSrc(tok, v.o, true);
            EndStatement();
            Emit(OP_SWITCH, tok);
            loops_.push_back({nullptr, true});
            for (auto& [labels, body] : s->cases) {
                for (s64 l : labels) {
                    if (l == INT64_MIN) {
                        Emit(OP_DEFAULT, {});
                    } else {
                        std::vector<u32> lt;
                        PutSrc(lt, Imm({B::Int, 1}, u32(l)).o, true);
                        Emit(OP_CASE, lt);
                    }
                }
                scopes_.emplace_back();
                for (auto& b : body)
                    CompileStmt(b.get());
                scopes_.pop_back();
            }
            loops_.pop_back();
            Emit(OP_ENDSWITCH, {});
            PropagateReturn(s);
            return;
        }
        case Stmt::Break:
            if (!loops_.empty() && loops_.back().unrolled)
                Fail("break in an unrolled loop");
            Emit(OP_BREAK, {});
            return;
        case Stmt::Continue: {
            // run the for-increment first: DXBC's continue jumps straight to the loop start
            for (auto it = loops_.rbegin(); it != loops_.rend(); ++it)
                if (!it->is_switch) {
                    if (it->unrolled)
                        Fail("continue in an unrolled loop");
                    if (it->inc) {
                        Eval(it->inc);
                        EndStatement();
                    }
                    break;
                }
            Emit(OP_CONTINUE, {});
            return;
        }
        case Stmt::Discard: {
            std::vector<u32> tok;
            PutSrc(tok, Imm({B::Uint, 1}, ~0u).o, true);
            Emit(OP_DISCARD, tok, kTestNonZero);
            return;
        }
        case Stmt::Return:
            if (s->e && in_main_ && !main_ret_struct_.empty()) {
                // `return o;`: the members are already in their temps (copied to the outputs at the end)
            } else if (s->e) {
                if (!cur_fn_ || cur_fn_->f->ret.b == B::Void)
                    Fail("return value outside a function");
                Store(cur_fn_->ret, Plain(Convert(Eval(s->e.get()), cur_fn_->ret.t)), nullptr);
                EndStatement();
            }
            if (frames_.empty()) { // main: ends the thread
                Emit(OP_RET, {});
                return;
            }
            if (NestedBreakables() > 0)
                Store(frames_.back().flag, Imm({B::Bool, 1}, ~0u), nullptr);
            Emit(OP_BREAK, {});
            return;
        }
    }
    void CompileFor(const Stmt* s) {
        scopes_.emplace_back();
        const Stmt* init = s->body[0].get();
        // [unroll] with a constant trip count: for (uint i = A; i < N; i++)
        if (s->unroll && init && init->k == Stmt::Decl && init->decls.size() == 1 && init->decls[0].init &&
            init->decls[0].init->k == Expr::Lit && s->e && s->e->k == Expr::Binary && s->e->op == "<" &&
            s->e->a[0]->k == Expr::Name && s->e->a[0]->op == init->decls[0].name && s->e->a[1]->k == Expr::Lit &&
            s->inc && s->inc->k == Expr::IncDec && s->inc->op == "++") {
            const u32 from = init->decls[0].init->lit, to = s->e->a[1]->lit;
            if (to - from > 64)
                Fail("unroll too long");
            for (u32 i = from; i < to; i++) {
                VarLoc c;
                c.k = VarLoc::Const;
                c.t = init->decls[0].t;
                c.constant = Imm(init->decls[0].t, i);
                scopes_.back()[init->decls[0].name] = c;
                loops_.push_back({nullptr, false, true});
                CompileStmt(s->body[1].get());
                loops_.pop_back();
            }
            scopes_.pop_back();
            return;
        }
        CompileStmt(init);
        Emit(OP_LOOP, {});
        if (s->e) {
            Val c = Cond(s->e.get());
            std::vector<u32> tok;
            PutSrc(tok, c.o, true);
            EndStatement();
            Emit(OP_BREAK + 1, tok, 0); // breakc_z
        }
        loops_.push_back({s->inc.get(), false});
        CompileStmt(s->body[1].get());
        loops_.pop_back();
        if (s->inc) {
            Eval(s->inc.get());
            EndStatement();
        }
        Emit(OP_ENDLOOP, {});
        PropagateReturn(s);
        scopes_.pop_back();
    }

    // ---------------- entry-point outputs
    // Output register of member i: vertex shaders in order (o0 = SV_Position, o1.. = ATTRn, as FXC assigns them),
    // pixel shaders by target index; SV_Depth goes to oDepth (~0u).
    u32 OutReg(u32 i) const {
        const StructMember& m = out_members_[i];
        if (stage_ == Stage::Pixel) {
            if (m.semantic == "SV_Depth")
                return ~0u;
            if (m.semantic.rfind("SV_Target", 0) == 0)
                return m.semantic.size() > 9 ? u32(std::stoul(m.semantic.substr(9))) : 0u;
            Fail("pixel shader output " + m.semantic);
        }
        return i;
    }
    void WriteOutputs() {
        for (u32 i = 0; i < out_vars_.size(); i++) {
            const VarLoc& v = out_vars_[i];
            const u32 reg = OutReg(i);
            std::vector<u32> tok;
            if (reg == ~0u)
                PutDst(tok, {OT_OUTPUT_DEPTH, 0, 1});
            else
                PutDst(tok, {OT_OUTPUT, reg, u8((1u << v.t.n) - 1)});
            Val val = Read(v);
            PutSrc(tok, val.o, reg == ~0u);
            Emit(OP_MOV, tok);
        }
    }
    static u32 CompType(B b) { return b == B::Float ? 3u : b == B::Int ? 2u : 1u; }
    // Splits "ATTR12" into ("ATTR", 12); system values keep their name with index 0 ("SV_Target3" -> 3).
    static std::pair<std::string, u32> SplitSemantic(const std::string& sem) {
        size_t e = sem.size();
        while (e > 0 && std::isdigit(u8(sem[e - 1])))
            e--;
        return {sem.substr(0, e), e < sem.size() ? u32(std::stoul(sem.substr(e))) : 0u};
    }
    void DeclareIo(std::vector<u32>& d) {
        auto dcl_reg = [&](u32 op, u32 ctrl, u32 type, u32 reg, u8 mask, int name) {
            d.push_back(op | ctrl | (u32(name >= 0 ? 4 : 3) << 24));
            d.push_back(2u | (u32(mask) << 4) | (type << 12) | (1u << 20));
            d.push_back(reg);
            if (name >= 0)
                d.push_back(u32(name));
        };
        for (u32 i = 0; i < in_members_.size(); i++) {
            const StructMember& m = in_members_[i];
            const u8 mask = u8((1u << m.t.n) - 1);
            auto [sname, sidx] = SplitSemantic(m.semantic);
            SigElement el{sname, sidx, 0, CompType(m.t.b), i, mask, mask};
            if (stage_ == Stage::Vertex) {
                if (m.semantic == "SV_VertexID" || m.semantic == "SV_InstanceID") {
                    el.sys_value = m.semantic == "SV_VertexID" ? 6 : 8;
                    dcl_reg(OP_DCL_INPUT_SGV, 0, OT_INPUT, i, mask, int(el.sys_value));
                } else {
                    dcl_reg(OP_DCL_INPUT, 0, OT_INPUT, i, mask, -1);
                }
            } else {
                // interpolation (opcode bits 11-14): 1 constant, 2 linear, 4 linear noperspective
                if (m.semantic == "SV_Position") {
                    el.sys_value = 1;
                    dcl_reg(OP_DCL_INPUT_PS_SIV, 4u << 11, OT_INPUT, i, mask, 1);
                } else if (m.semantic == "SV_IsFrontFace") {
                    el.sys_value = 9;
                    dcl_reg(OP_DCL_INPUT_PS_SGV, 1u << 11, OT_INPUT, i, mask, 9);
                } else {
                    const u32 mode = m.t.b != B::Float || m.interp == 1 ? 1u : m.interp == 3 ? 4u : 2u;
                    dcl_reg(OP_DCL_INPUT_PS, mode << 11, OT_INPUT, i, mask, -1);
                }
            }
            if (el.sys_value == 9 || el.sys_value == 6 || el.sys_value == 8)
                el.rw_mask = 0; // (as FXC writes them)
            isgn.push_back(el);
        }
        for (u32 i = 0; i < out_members_.size(); i++) {
            const StructMember& m = out_members_[i];
            const u8 mask = u8((1u << m.t.n) - 1);
            const u32 reg = OutReg(i);
            auto [sname, sidx] = SplitSemantic(m.semantic);
            SigElement el{sname, sidx, 0, CompType(m.t.b), reg, mask, 0};
            if (reg == ~0u) {
                d.push_back(OP_DCL_OUTPUT | (2u << 24));
                d.push_back(1u | (OT_OUTPUT_DEPTH << 12));
                el.mask = 1;
                el.rw_mask = 0xE;
            } else if (m.semantic == "SV_Position") {
                el.sys_value = 1;
                dcl_reg(OP_DCL_OUTPUT_SIV, 0, OT_OUTPUT, reg, mask, 1);
            } else {
                dcl_reg(OP_DCL_OUTPUT, 0, OT_OUTPUT, reg, mask, -1);
            }
            osgn.push_back(el);
        }
    }

    // ---------------- declarations and assembly
    std::vector<u32> Assemble(const Func& main) {
        std::vector<u32> d;
        auto dcl = [&](u32 op, std::initializer_list<u32> toks, u32 ctrl = 0) {
            d.push_back(op | ctrl | (u32(1 + toks.size()) << 24));
            for (u32 t : toks)
                d.push_back(t);
        };
        dcl(OP_DCL_GLOBAL_FLAGS, {}, 1u << 11); // refactoringAllowed
        for (auto& [slot, rows] : p_.cb_size) {
            const bool dyn = cb_dynamic_.count(slot) != 0;
            dcl(OP_DCL_CONSTANT_BUFFER, {0x00208e46u, slot, std::max<u32>(rows, 1)}, dyn ? (1u << 11) : 0u);
        }
        for (auto& [name, r] : p_.resources) {
            switch (r.kind) {
            case ResKind::Sampler:
                dcl(OP_DCL_SAMPLER, {0x00106000u, r.reg}, r.comparison ? (1u << 11) : 0u);
                break;
            case ResKind::RawBuffer:
                dcl(OP_DCL_RESOURCE_RAW, {0x00107000u, r.reg});
                break;
            case ResKind::RWRawBuffer:
                dcl(OP_DCL_UAV_RAW, {0x0011e000u, r.reg});
                break;
            case ResKind::Texture: {
                const u32 rt = RetType(r.elem.b);
                dcl(OP_DCL_RESOURCE, {0x00107000u, r.reg, rt | (rt << 4) | (rt << 8) | (rt << 12)}, ResDim(r.dim) << 11);
                break;
            }
            }
        }
        for (u32 in : inputs_)
            dcl(OP_DCL_INPUT, {2u | (0x7u << 4) | (in << 12)});
        if (stage_ != Stage::Compute)
            DeclareIo(d);
        dcl(OP_DCL_TEMPS, {TempCount()});
        for (u32 i = 0; i < itemps_.size(); i++)
            dcl(OP_DCL_INDEXABLE_TEMP, {i, itemps_[i].count, 4});
        for (u32 i = 0; i < tgsm_.size(); i++)
            dcl(OP_DCL_TGSM_STRUCTURED, {0x0011f000u, i, 4, tgsm_[i]});
        if (stage_ == Stage::Compute)
            dcl(OP_DCL_THREAD_GROUP, {main.numthreads[0], main.numthreads[1], main.numthreads[2]});
        std::vector<u32> out;
        // program type (0 pixel, 1 vertex, 5 compute) << 16, shader model 5.0
        out.push_back(stage_ == Stage::Pixel ? 0x00000050u : stage_ == Stage::Vertex ? 0x00010050u : 0x00050050u);
        out.push_back(0);
        out.insert(out.end(), d.begin(), d.end());
        out.insert(out.end(), code_.begin(), code_.end());
        out[1] = u32(out.size());
        return out;
    }

public:
    // set by CallUser; declared here so FuncInfo stays a plain struct above
};

} // namespace

bool CompileHlsl(const std::string& source, Stage stage, std::vector<u8>& out, std::string& err) {
    try {
        Parser p(Lex(source));
        p.ParseProgram();
        Codegen g(p, stage);
        const std::vector<u32> shex = g.Run();
        out = Container(shex, g.isgn, g.osgn);
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

} // namespace gpu::dxbc
