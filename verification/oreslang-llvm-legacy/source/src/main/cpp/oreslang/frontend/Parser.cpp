#include "oreslang/frontend/Parser.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace oreslang {
namespace {

enum class Kind { Identifier, Integer, Symbol, End };
struct Token {
    Kind kind;
    std::string_view text;
    std::size_t offset;
};

bool digit(char ch) { return ch >= '0' && ch <= '9'; }
bool identifier_start(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}
bool identifier_part(char ch) { return identifier_start(ch) || digit(ch); }

bool reserved(std::string_view name) {
    // Keep these reserved even while their grammar is unsupported.
    static constexpr std::string_view words[] = {
        "define", "class", "module", "contract", "conforms", "namespace",
        "import", "from", "as", "extends", "implements", "try", "catch",
        "finally", "end", "fi", "if", "do", "else", "then", "new",
        "spawn", "stop", "done", "await", "rt", "async", "generator",
        "pipeline", "expect_pipeline", "cancel_pipeline",
        "nlex", "trap", "actor", "isoactor", "def", "fnc", "routine",
        "for", "of", "loop", "block", "break", "continue", "yield",
        "super", "elseif", "elif", "switch", "match", "matches", "is",
        "default", "eq", "neq", "nb", "select", "readch", "writech",
        "cb", "shared", "untrusted", "type", "types", "typeof",
        "interface", "trait", "struct", "impl", "abstract", "void",
        "static", "pub", "private", "return", "defer", "val", "const",
        "let", "mut", "self", "true", "false", "null", "obj", "arr",
        "int", "owned", "declare", "receive", "receive_forward",
        "receive_backward", "transform", "pipe", "forward_messages",
        "backward_messages", "Message", "bool", "string", "state",
        "critical", "handle_state", "using", "with",
        "Array", "List"
    };
    return std::find(std::begin(words), std::end(words), name) != std::end(words);
}

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {}

    Token next() {
        for (;;) {
            if (at_ == source_.size()) return {Kind::End, {}, at_};
            const unsigned char ch = static_cast<unsigned char>(source_[at_]);
            if (std::isspace(ch)) { ++at_; continue; }
            if (source_[at_] == '#') {
                const auto end = source_.find('\n', at_ + 1);
                at_ = end == std::string_view::npos ? source_.size() : end;
                continue;
            }
            if (source_.substr(at_, 2) == "//") {
                const auto end = source_.find('\n', at_ + 2);
                at_ = end == std::string_view::npos ? source_.size() : end;
                continue;
            }
            if (source_.substr(at_, 2) == "/*") {
                const auto start = at_;
                at_ += 2;
                std::size_t depth = 1;
                while (depth != 0) {
                    if (at_ == source_.size()) {
                        throw std::runtime_error("unterminated block comment at byte "
                                                 + std::to_string(start));
                    }
                    if (source_.substr(at_, 2) == "/*") { at_ += 2; ++depth; }
                    else if (source_.substr(at_, 2) == "*/") { at_ += 2; --depth; }
                    else ++at_;
                }
                continue;
            }
            break;
        }
        const std::size_t start = at_;
        const char ch = source_[at_];
        if (identifier_start(ch)) {
            ++at_;
            while (at_ < source_.size() && identifier_part(source_[at_])) ++at_;
            return {Kind::Identifier, source_.substr(start, at_ - start), start};
        }
        if (digit(ch)) {
            ++at_;
            while (at_ < source_.size() && (digit(source_[at_]) || source_[at_] == '_')) {
                ++at_;
            }
            return {Kind::Integer, source_.substr(start, at_ - start), start};
        }
        if (ch == '-' && at_ + 1 < source_.size() && source_[at_ + 1] == '>') {
            at_ += 2;
            return {Kind::Symbol, source_.substr(start, 2), start};
        }
        if (ch == ':' && at_ + 1 < source_.size() && source_[at_ + 1] == ':') {
            at_ += 2;
            return {Kind::Symbol, source_.substr(start, 2), start};
        }
        if (std::string_view("()[]{},:;+-*=.<>|").find(ch) != std::string_view::npos) {
            ++at_;
            return {Kind::Symbol, source_.substr(start, 1), start};
        }
        throw std::runtime_error("unsupported Oreslang token at byte "
                                 + std::to_string(start));
    }

private:
    std::string_view source_;
    std::size_t at_ = 0;
};

std::unique_ptr<Expression> make_expression(Expression::Kind kind,
                                             std::size_t offset) {
    auto result = std::make_unique<Expression>();
    result->kind = kind;
    result->offset = offset;
    return result;
}

class Parser {
public:
    explicit Parser(std::string_view source) : lexer_(source), token_(lexer_.next()) {}

    Program parse() {
        Program program;
        bool wrapped_module = false;
        if (accept("define")) {
            if (accept("module")) {
                program.module = identifier();
                accept("as");
                wrapped_module = true;
            } else if (accept("actor")) {
                program.actors.push_back(actor());
            } else if (accept("transform")) {
                require("pipe"); require("actor");
                program.actors.push_back(actor(true));
            } else if (accept("enum")) {
                program.enums.push_back(tagged_enum());
            } else if (accept("match")) {
                require("pattern");
                program.tagged_patterns.push_back(tagged_pattern());
            } else {
                fail("expected 'module', 'actor', 'enum' or 'match pattern'");
            }
        }
        while (token_.kind != Kind::End
                && !(wrapped_module && token_.text == "end")) {
            if (accept("declare")) {
                program.message_patterns.push_back(message_pattern());
            } else if (accept("define")) {
                if (accept("actor")) program.actors.push_back(actor());
                else if (accept("transform")) {
                    require("pipe"); require("actor");
                    program.actors.push_back(actor(true));
                }
                else if (accept("enum")) program.enums.push_back(tagged_enum());
                else if (accept("match")) {
                    require("pattern");
                    program.tagged_patterns.push_back(tagged_pattern());
                }
                else fail("expected 'actor', 'enum' or 'match pattern'");
            } else {
                program.functions.push_back(function());
            }
        }
        if (wrapped_module) require("end");
        if (token_.kind != Kind::End) fail("unexpected trailing source");
        if (program.functions.empty() && program.actors.empty()
            && program.message_patterns.empty() && program.enums.empty()
            && program.tagged_patterns.empty())
            fail("expected a function or actor");
        validate(program);
        return program;
    }

private:
    // The tagged source grammar is intentionally declaration-only for now.
    // Actual guest send/receive matching still needs compile-time lowering to
    // the canonical MessagePack v2 source ABI; see validate() below.
    EnumDeclaration tagged_enum() {
        EnumDeclaration result;
        result.offset = token_.offset;
        result.name = identifier();
        require("as");
        std::unordered_set<std::string> caseNames;
        std::unordered_set<std::uint32_t> usedTags;
        std::uint64_t next = 1;
        while (token_.kind != Kind::End && token_.text != "end") {
            EnumCase item;
            item.name = identifier();
            if (!caseNames.insert(item.name).second)
                fail("duplicate enum case");
            if (accept(":")) {
                item.has_payload = true;
                item.payload = message_shape();
                // Strictly canonical tagged envelope shapes:
                // array/list is homogeneous, fixed sequences carry slots.
                if ((item.payload.kind == MessageShape::Kind::Array
                     || item.payload.kind == MessageShape::Kind::List)
                    && item.payload.elements.size() != 1)
                    fail("variable enum message collection must have exactly one element type");
            }
            std::uint64_t tag = next;
            if (accept("=")) {
                if (token_.kind != Kind::Integer) fail("expected positive enum tag");
                std::string digits(token_.text);
                if (digits.front() == '_' || digits.back() == '_'
                    || digits.find("__") != std::string::npos)
                    fail("invalid enum wire tag");
                digits.erase(std::remove(digits.begin(), digits.end(), '_'), digits.end());
                const auto parsed = std::from_chars(digits.data(),
                    digits.data() + digits.size(), tag);
                if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size())
                    fail("enum wire tag overflow");
                advance();
            }
            if (tag == 0 || tag > 2147483647ULL)
                fail("enum wire tag must be a positive signed 32-bit integer");
            if (tag < next) fail("enum wire tags must increase monotonically");
            if (!usedTags.insert(static_cast<std::uint32_t>(tag)).second)
                fail("duplicate enum wire tag");
            item.tag = static_cast<std::uint32_t>(tag);
            next = tag + 1;
            if (result.cases.size() >= 4096)
                fail("tagged enum exceeds 4096 alternatives");
            result.cases.push_back(std::move(item));
            accept(",");
            accept(";");
        }
        if (result.cases.empty()) fail("enum must declare at least one case");
        require("end");
        return result;
    }

    TaggedPattern tagged_pattern() {
        TaggedPattern result;
        result.offset = token_.offset;
        result.name = identifier();
        require("as");
        std::unordered_set<std::string> seen;
        while (token_.kind != Kind::End && token_.text != "end") {
            const std::string owner = identifier();
            require(".");
            const std::string variant = identifier();
            const std::string qualified = owner + "." + variant;
            if (!seen.insert(qualified).second)
                fail("duplicate pattern alternative");
            if (result.alternatives.size() >= 4096)
                fail("tagged pattern exceeds 4096 alternatives");
            result.alternatives.push_back(qualified);
            accept(",");
            accept(";");
        }
        if (result.alternatives.empty()) fail("pattern must have at least one alternative");
        require("end");
        return result;
    }

    MessageShape message_shape(unsigned depth = 0, std::size_t* shape_nodes = nullptr) {
        // Match the shared codec's total descriptor node budget, not just
        // each individual nested sequence's immediate arity. A single huge
        // tree must not bypass the resource cap via many small subtrees.
        std::size_t root_nodes = 0;
        if (!shape_nodes) shape_nodes = &root_nodes;
        if (depth > 64) fail("message shape exceeds maximum type nesting depth");
        if (++*shape_nodes > 100000)
            fail("message shape exceeds total descriptor node budget");
        if (accept("bool")) return {MessageShape::Kind::Boolean, {}};
        if (accept("int")) return {MessageShape::Kind::Integer, {}};
        if (accept("string")) return {MessageShape::Kind::String, {}};
        MessageShape shape;
        std::string_view close;
        if (accept("(")) {
            shape.kind = MessageShape::Kind::Tuple;
            close = ")";
        } else if (accept("Tuple")) {
            shape.kind = MessageShape::Kind::Tuple;
            require("[");
            close = "]";
        } else if (accept("FixedArray")) {
            shape.kind = MessageShape::Kind::FixedArray;
            require("[");
            close = "]";
        } else if (accept("FixedList")) {
            shape.kind = MessageShape::Kind::FixedList;
            require("[");
            close = "]";
        } else if (accept("Array")) {
            shape.kind = MessageShape::Kind::Array;
            require("[");
            close = "]";
        } else if (accept("List")) {
            shape.kind = MessageShape::Kind::List;
            require("[");
            close = "]";
        } else {
            fail("expected message shape (bool, int, string, Tuple, FixedArray, FixedList, Array or List)");
        }
        if (accept(close))
            fail("empty structural message pattern is not supported");
        do {
            if (shape.elements.size() >= 4096)
                fail("message shape exceeds maximum descriptor elements");
            shape.elements.push_back(message_shape(depth + 1, shape_nodes));
        } while (accept(","));
        require(close);
        if ((shape.kind == MessageShape::Kind::Array
             || shape.kind == MessageShape::Kind::List)
            && shape.elements.size() != 1)
            fail("variable enum message collection must have exactly one element type");
        return shape;
    }

    MessagePattern message_pattern() {
        MessagePattern pattern;
        pattern.offset = token_.offset;
        require("type");
        require("match");
        pattern.name = identifier();
        require("as");
        while (token_.kind != Kind::End && token_.text != "end") {
            pattern.alternatives.push_back(message_shape());
            accept(";");
        }
        if (pattern.alternatives.empty())
            fail("message match contract must declare at least one alternative");
        require("end");
        return pattern;
    }

    Actor actor(bool transform_pipe = false) {
        Actor value;
        value.offset = token_.offset;
        value.transform_pipe = transform_pipe;
        value.name = identifier();
        if (transform_pipe) {
            require("<");
            // Both directional input/output domains are required. The first
            // native profile uses one domain per direction, not implicit Message.
            for (unsigned i = 0; i < 2; ++i) {
                if (accept("forward_messages")) {
                    if (!value.forward_messages_type.empty())
                        fail("duplicate forward_messages domain");
                    require("="); value.forward_messages_type = identifier();
                } else if (accept("backward_messages")) {
                    if (!value.backward_messages_type.empty())
                        fail("duplicate backward_messages domain");
                    require("="); value.backward_messages_type = identifier();
                } else fail("transform pipe actor requires forward_messages and backward_messages");
                if (i == 0) require(",");
            }
            require(">");
            if (value.forward_messages_type.empty() || value.backward_messages_type.empty())
                fail("transform pipe actor requires both directional domains");
        } else if (accept("<")) {
            require("messages");
            require("=");
            value.messages_type = identifier();
            require(">");
        }
        require("as");
        const bool braced = accept("{");
        // Actor data lives only in a single declared fixed-key state map.
        // Ordinary class fields are never admitted on an actor, even when
        // private, immutable, or declared before all methods.
        if (accept("state")) {
            value.state_declared = true;
            require("{");
            while (!accept("}")) {
                if (token_.kind == Kind::End || value.fields.size() >= 64)
                    fail("actor state requires closing '}' and at most 64 keys");
                LocalBinding field;
                field.offset = token_.offset;
                field.name = identifier();
                require(":");
                require("int");
                require("=");
                field.initializer = expression();
                require(";");
                field.field_mutable = true;
                value.fields.push_back(std::move(field));
            }
            accept(";");
        }
        if (token_.text == "const" || token_.text == "val"
                || token_.text == "let" || token_.text == "state")
            fail("actor instance data must be declared in one state map");
        while (token_.text == "fnc" || token_.text == "routine"
               || token_.text == "pub" || token_.text == "receive"
               || token_.text == "receive_forward" || token_.text == "receive_backward") {
            value.methods.push_back(function(true,
                transform_pipe ? value.forward_messages_type : value.messages_type,
                value.backward_messages_type, transform_pipe));
        }
        if (value.methods.empty()) fail("actor requires at least one read-only method");
        if (braced) require("}");
        else require("end");
        return value;
    }

    // Inline spawns are admitted into the same owned reference list as named
    // spawns, so all mailbox, route and teardown validation remains shared.
    std::string pipeline_stage(Function& fn) {
        if (!accept("spawn")) return identifier();
        ActorSpawnBinding ref;
        ref.offset = token_.offset;
        ref.name = "$pipe" + std::to_string(fn.actor_spawns.size());
        ref.actor_type = identifier();
        ref.declared_type.kind = SourceType::Kind::ActorRef;
        ref.declared_type.argument = ref.actor_type;
        require("("); require(")");
        std::string name = ref.name;
        fn.actor_spawns.push_back(std::move(ref));
        fn.actor_stops.insert(fn.actor_stops.begin(), name);
        return name;
    }

    std::unique_ptr<StateLoan> parse_scoped_state_loan() {
        require("const");
        auto loan = std::make_unique<StateLoan>();
        loan->result_binding = identifier();
        require("=");
        if (accept("using")) {
            require("state"); require(".");
            if (accept("read"))
                loan->mode = StateLoan::Mode::Read;
            else if (accept("write"))
                loan->mode = StateLoan::Mode::Write;
            else fail("using state capability must be read or write");
            require("(");
            if (loan->mode == StateLoan::Mode::Write)
                require("critical");
            else if (token_.text == "critical")
                fail("using state.read cannot request a critical write loan");
        } else {
            require("self"); require(".");
            require("handle_state"); require("(");
            loan->mode = accept("critical")
                ? StateLoan::Mode::Write
                : StateLoan::Mode::Read;
        }
        require("|"); require("state"); require("|");
        require("->"); require("{");
        state_loan_depth_ = 1;
        while (token_.text == "state") {
            if (loan->mode == StateLoan::Mode::Read)
                fail("read-only actor state loan cannot mutate state");
            FieldAssignment assignment;
            assignment.offset = token_.offset;
            require("state"); require(".");
            assignment.field = identifier();
            require("=");
            assignment.value = expression();
            require(";");
            loan->assignments.push_back(std::move(assignment));
        }
        if (token_.text == "await" || token_.text == "select"
                || token_.text == "nb" || token_.text == "using")
            fail("state read/write loan cannot suspend or nest");
        require("return");
        loan->result = expression();
        require(";");
        state_loan_depth_ = 0;
        require("}"); require(")"); require(";");
        return loan;
    }

    Function function(bool actor_method = false,
                      const std::string& message_type = "",
                      const std::string& backward_type = "",
                      bool transform_pipe = false) {
        Function value;
        value.offset = token_.offset;
        value.exported = accept("pub");
        if (actor_method && (token_.text == "receive"
                || token_.text == "receive_forward"
                || token_.text == "receive_backward")) {
            value.is_receiver = true;
            value.name = std::string(token_.text);
            advance();
            value.exported = true;
            if (transform_pipe != (value.name != "receive"))
                fail("transform pipe actor requires directional receive handlers");
        } else {
            if (!accept("fnc") && !accept("routine")) fail("expected fnc or routine");
            value.name = identifier();
        }
        require("(");
        if (value.is_receiver) {
            const std::string& domain =
                value.name == "receive_backward" ? backward_type : message_type;
            if (domain.empty() || token_.text != domain)
                fail(value.name + " parameter type must match declared message domain '" + domain + "'");
            require(domain);
            value.parameters.push_back(identifier());
            require(")");
            if (transform_pipe) { require(":"); require("void"); }
        } else {
            if (!accept(")")) {
                do {
                    require("int");
                    value.parameters.push_back(identifier());
                } while (accept(","));
                require(")");
            }
            require(":");
            require("int");
        }
        // "with" is a declaration of permitted effects, not a loan.
        // "using" remains a scoped expression at the actual access site.
        value.effects_declared = accept("with");
        if (value.effects_declared) {
            do {
                require("state"); require(".");
                const auto effect = identifier();
                if (effect != "read" && effect != "write")
                    fail("unsupported with effect in restricted native profile");
                const std::string named = "state." + effect;
                if (std::find(value.declared_effects.begin(),
                              value.declared_effects.end(), named)
                        != value.declared_effects.end())
                    fail("duplicate with state effect");
                value.declared_effects.push_back(named);
            } while (accept(","));
        }
        if (token_.text == "using")
            fail("using acquires scoped capabilities; use with for function effects");
        require("{");
        bool scalar_local_seen = false;
        while (token_.text == "const" || token_.text == "val") {
            // A method-level const binding of an actor-owned state callback
            // is handled as a sealed loan, not an ordinary scalar initializer.
            if (actor_method && !value.is_receiver && token_.text == "const") {
                Lexer probe = lexer_;
                const Token next = probe.next(), separator = probe.next();
                if (next.kind == Kind::Identifier && separator.text == "=") break;
            }
            LocalBinding binding;
            binding.offset = token_.offset;
            advance();
            binding.name = identifier();
            const bool has_explicit_type = accept(":");
            SourceType declared_type;
            bool actor_ref_type = false;
            if (has_explicit_type) {
                if (accept("ActorRef")) {
                    require("<");
                    declared_type.kind = SourceType::Kind::ActorRef;
                    declared_type.argument = identifier();
                    require(">");
                    actor_ref_type = true;
                } else if (accept("Future")) {
                    // Reserve a genuine typed Future category without
                    // creating a fake synchronous future value or await.
                    require("<");
                    declared_type.kind = SourceType::Kind::Future;
                    if (accept("int")) declared_type.argument = "int";
                    else declared_type.argument = identifier();
                    require(">");
                    fail("Future<T> requires native continuation lowering");
                } else {
                    binding.heap_owned = accept("owned");
                    require("int");
                }
            }
            require("=");
            if (token_.text == "await")
                fail("await requires native compiler continuation lowering");
            if (accept("spawn")) {
                if (actor_method || scalar_local_seen
                    || (has_explicit_type && !actor_ref_type))
                    fail("actor spawn requires ActorRef<T> or inferred reference in main");
                ActorSpawnBinding actor_ref;
                actor_ref.offset = binding.offset;
                actor_ref.name = std::move(binding.name);
                actor_ref.actor_type = identifier();
                actor_ref.declared_type.kind = SourceType::Kind::ActorRef;
                actor_ref.declared_type.argument = actor_ref_type
                    ? declared_type.argument : actor_ref.actor_type;
                if (actor_ref.declared_type.argument != actor_ref.actor_type)
                    fail("ActorRef<T> type mismatch with spawned actor");
                require("(");
                require(")");
                require(";");
                value.actor_spawns.push_back(std::move(actor_ref));
            } else {
                if (actor_ref_type)
                    fail("ActorRef<T> requires spawn initializer");
                if (!has_explicit_type) fail("scalar local requires ': int'");
                scalar_local_seen = true;
                binding.initializer = expression();
                require(";");
                value.locals.push_back(std::move(binding));
            }
        }
        if (!actor_method) {
            bool pipe_call = accept("pipeline");
            if (!pipe_call && token_.text == "ActorUtils") {
                Lexer probe = lexer_;
                const auto delimiter = probe.next().text;
                if ((delimiter == "." || delimiter == "::") && probe.next().text == "pipe") {
                    if (delimiter == "." && std::any_of(value.actor_spawns.begin(),
                            value.actor_spawns.end(), [](const ActorSpawnBinding& ref) {
                                return ref.name == "ActorUtils";
                            })) fail("ambiguous ActorUtils.pipe; use ActorUtils::pipe");
                    advance();
                    if (!accept(".")) require("::");
                    require("pipe");
                    pipe_call = true;
                }
            }
            bool pipe_operator = token_.text == "spawn";
            if (!pipe_call && token_.kind == Kind::Identifier) {
                Lexer probe = lexer_;
                pipe_operator = pipe_operator || probe.next().text == "pipe";
            }
            if (!pipe_call && pipe_operator) {
                value.pipeline_actors.push_back(pipeline_stage(value));
                require("pipe");
                do { value.pipeline_actors.push_back(pipeline_stage(value)); }
                while (accept("pipe"));
                require(";");
                if (value.pipeline_actors.size() < 2 || value.pipeline_actors.size() > 16)
                    fail("pipeline requires between 2 and 16 ordered actors");
            }
            if (pipe_call) {
                require("(");
                do { value.pipeline_actors.push_back(pipeline_stage(value)); }
                while (accept(","));
                require(")");
                require(";");
                if (value.pipeline_actors.size() < 2
                    || value.pipeline_actors.size() > 16)
                    fail("pipeline requires between 2 and 16 ordered actors");
            }
            if (accept("cancel_pipeline")) {
                require("(");
                value.pipeline_cancel_actor = identifier();
                require(")");
                require(";");
            }
            if (accept("await")) {
                value.ready_await_actor = identifier();
                require(".");
                require("get_ready");
                require("(");
                require(")");
                require(";");
            }
            while (token_.kind == Kind::Identifier && !reserved(token_.text)) {
                ActorSend send;
                send.offset = token_.offset;
                send.reference = identifier();
                require(".");
                require("send");
                require("(");
                // The restricted source-compiled nominal send form is
                // ref.send(Enum.Variant), without an implicit scalar cast.
                // Copy the lexical cursor to avoid consuming scalar
                // expressions or interpreting a function call as a case.
                Lexer probe = lexer_;
                Token next = probe.next();
                if (token_.kind == Kind::Identifier && !reserved(token_.text)
                        && next.text == ".") {
                    const std::string enum_name = identifier();
                    require(".");
                    send.tagged_variant = enum_name + "." + identifier();
                    /* First executable payload subset: Enum.Case(123).
                     * The literal is encoded at compile time, never cast
                     * into an actor's scalar i64 mail channel. */
                    if (accept("(")) {
                        send.payload = expression();
                        require(")");
                    }
                } else {
                    send.payload = expression();
                }
                require(")");
                require(";");
                value.actor_sends.push_back(std::move(send));
            }
            if (accept("expect_pipeline")) {
                require("(");
                value.pipeline_expect_actor = identifier();
                require(",");
                if (token_.kind != Kind::Integer)
                    fail("expect_pipeline needs nonnegative int literal");
                std::string digits(token_.text);
                if (digits.empty() || digits.front() == '_' || digits.back() == '_'
                    || digits.find("__") != std::string::npos)
                    fail("invalid expect_pipeline integer");
                digits.erase(std::remove(digits.begin(), digits.end(), '_'), digits.end());
                auto parsed = std::from_chars(digits.data(),
                    digits.data() + digits.size(), value.pipeline_expect_value);
                if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size())
                    fail("expect_pipeline expected int out of range");
                advance();
                require(")");
                require(";");
            }
            while (accept("stop")) {
                value.actor_stops.push_back(identifier());
                require(";");
            }
        }
        if (token_.text == "await")
            fail("await requires native compiler continuation lowering");
        // Source-only tagged arm admission shares the canonical syntax with
        // Java, but never emits fake native i64 match branches. Only empty
        // arm bodies are accepted here: work and effects inside an arm need
        // their own explicit ownership-aware LLVM lowering.
        if (value.is_receiver && (token_.text == "match" || token_.text == "do")) {
            const bool procedural = accept("do");
            require("match");
            value.match_subject = identifier();
            if (accept("over")) {
                require("{");
                while (token_.text != "}") {
                    if (token_.kind == Kind::End)
                        fail("unterminated tagged receive match");
                    TaggedReceiveArm arm;
                    arm.offset = token_.offset;
                    require("on");
                    const std::string owner = identifier();
                    require(".");
                    arm.qualified_case = owner + "." + identifier();
                    if (accept("as")) {
                        arm.binds_payload = true;
                        if (accept("(")) {
                            arm.tuple_destructure = true;
                            if (!accept(")")) {
                                do { arm.binders.push_back(identifier()); }
                                while (accept(","));
                                require(")");
                            }
                        } else {
                            arm.binders.push_back(identifier());
                        }
                    }
                    require("{");
                    // Canonical actor state API:
                    // self.handle_state(|state| -> {...})          -> read loan
                    // self.handle_state(critical |state| -> {...}) -> write loan.
                    // The earlier using state.read/write syntax is retained
                    // only as a restricted equivalent for this migration.
                    // No loan may escape the callback or suspend.
                    if (token_.text == "const")
                        arm.state_loan = parse_scoped_state_loan();
                    // Legacy direct field mutation is intentionally rejected.
                    if (token_.kind == Kind::Identifier && !reserved(token_.text))
                        fail("actor state mutation requires scoped handle_state callback");
                    if (accept("self")) {
                        require(".");
                        if (accept("forward")) arm.route_direction = "forward";
                        else if (accept("backward")) arm.route_direction = "backward";
                        else fail("unsupported self pipeline operation");
                        require("(");
                        arm.route_variant = identifier();
                        require(".");
                        arm.route_variant += "." + identifier();
                        require("(");
                        arm.route_payload = expression();
                        require(")");
                        require(")");
                        require(";");
                    }
                    require("}");
                    value.tagged_arms.push_back(std::move(arm));
                    accept(";");
                    accept(",");
                }
                require("}");
            } else {
                if (procedural)
                    fail("do match requires 'over' and typed alternatives");
                // Existing executable bool-only matcher remains unchanged.
                require("{");
                while (token_.text != "}") {
                    if (token_.kind == Kind::End) fail("unterminated receive match");
                    ReceiveBooleanArm arm;
                    arm.offset = token_.offset;
                    if (accept("true")) arm.match_value = true;
                    else if (accept("false")) arm.match_value = false;
                    else fail("expected true/false match arm");
                    require("=");
                    require(">");
                    require("{");
                    while (token_.kind == Kind::Identifier && !reserved(token_.text)) {
                        FieldAssignment assignment;
                        assignment.offset = token_.offset;
                        assignment.field = identifier();
                        require("=");
                        assignment.value = expression();
                        require(";");
                        arm.assignments.push_back(std::move(assignment));
                    }
                    if (token_.text == "const")
                        arm.state_loan = parse_scoped_state_loan();
                    require("}");
                    value.boolean_arms.push_back(std::move(arm));
                    accept(";");
                }
                require("}");
            }
        }
        // Directional pipeline operations require a scheduler-issued route
        // context, not a bare native call or scalar actor-field assignment.
        // Emit a specific compile-time rejection until both the routing
        // context and continuation-aware guest lowering are implemented.
        if (actor_method && token_.text == "self") {
            Lexer probe = lexer_;
            const Token dot = probe.next();
            const Token operation = probe.next();
            if (dot.text == "." &&
                    (operation.text == "forward" || operation.text == "backward"
                     || operation.text == "respond" || operation.text == "done")) {
                fail(std::string("self.") + std::string(operation.text)
                    + " requires native pipeline routing lowering");
            }
        }
        // Restricted synchronous actor methods can update declared `let` fields.
        while (actor_method && token_.kind == Kind::Identifier
               && !reserved(token_.text)) {
            FieldAssignment assignment;
            assignment.offset = token_.offset;
            assignment.field = identifier();
            require("=");
            assignment.value = expression();
            require(";");
            value.assignments.push_back(std::move(assignment));
        }
        if (actor_method && !value.is_receiver && token_.text == "const")
            value.method_state_loan = parse_scoped_state_loan();
        if (value.is_receiver && (token_.text == "}" || (transform_pipe && token_.text == "return"))) {
            if (accept("return")) require(";");
            value.result = make_expression(Expression::Kind::Integer, token_.offset);
            value.result->integer = 0;
        } else {
            require("return");
            value.result = expression();
            require(";");
        }
        require("}");
        return value;
    }

    std::unique_ptr<Expression> expression() { return additive(); }

    std::unique_ptr<Expression> additive() {
        auto left = product();
        while (token_.text == "+" || token_.text == "-") {
            const auto offset = token_.offset;
            const auto op = token_.text;
            advance();
            auto expr = make_expression(op == "+" ? Expression::Kind::Add
                                                 : Expression::Kind::Subtract, offset);
            expr->left = std::move(left);
            expr->right = product();
            left = std::move(expr);
        }
        return left;
    }

    std::unique_ptr<Expression> product() {
        auto left = unary();
        while (token_.text == "*") {
            const auto offset = token_.offset;
            advance();
            auto expr = make_expression(Expression::Kind::Multiply, offset);
            expr->left = std::move(left);
            expr->right = unary();
            left = std::move(expr);
        }
        return left;
    }

    std::unique_ptr<Expression> unary() {
        if (token_.text == "-") {
            const auto offset = token_.offset;
            advance();
            /* The magnitude 2^63 does not fit a positive signed i64,
             * although -2^63 is a valid guest integer and MessagePack I.
             * Consume this one literal atomically without overflow or an
             * implementation-defined signed cast. Larger magnitudes still
             * fail in primary(), before emitting any IR. */
            if (token_.kind == Kind::Integer) {
                std::string digits(token_.text);
                if (!digits.empty() && digits.front() != '_'
                        && digits.back() != '_'
                        && digits.find("__") == std::string::npos) {
                    digits.erase(std::remove(digits.begin(), digits.end(), '_'),
                                 digits.end());
                    std::uint64_t magnitude = 0;
                    const auto parsed = std::from_chars(
                        digits.data(), digits.data() + digits.size(), magnitude);
                    if (parsed.ec == std::errc{}
                            && parsed.ptr == digits.data() + digits.size()
                            && magnitude == (UINT64_C(1) << 63)) {
                        auto result = make_expression(Expression::Kind::Integer, offset);
                        result->integer = INT64_MIN;
                        advance();
                        return result;
                    }
                }
            }
            auto expr = make_expression(Expression::Kind::Negate, offset);
            expr->left = unary();
            return expr;
        }
        return primary();
    }

    std::unique_ptr<Expression> primary() {
        const auto offset = token_.offset;
        if (accept("(")) {
            auto inner = expression();
            require(")");
            return inner;
        }
        if (token_.text == "true" || token_.text == "false") {
            auto expr = make_expression(Expression::Kind::Boolean, offset);
            expr->integer = token_.text == "true" ? 1 : 0;
            advance();
            return expr;
        }
        if (token_.kind == Kind::Integer) {
            auto expr = make_expression(Expression::Kind::Integer, offset);
            std::string digits(token_.text);
            if (digits.front() == '_' || digits.back() == '_'
                    || digits.find("__") != std::string::npos) {
                fail("malformed integer separator");
            }
            digits.erase(std::remove(digits.begin(), digits.end(), '_'), digits.end());
            const auto parsed = std::from_chars(digits.data(),
                                                 digits.data() + digits.size(),
                                                 expr->integer);
            if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) {
                fail("integer literal overflows signed 64-bit result");
            }
            advance();
            return expr;
        }
        if (token_.kind != Kind::Identifier
                || (reserved(token_.text)
                    && !(state_loan_depth_ && token_.text == "state"))) {
            fail("expected int expression");
        }
        if (state_loan_depth_ && token_.text == "state") {
            require("state");
            if (token_.text != ".")
                fail("actor state reference cannot escape handle_state callback");
            require(".");
            auto value = make_expression(Expression::Kind::Variable, offset);
            value->name = "@state." + identifier();
            return value;
        }
        auto expr = make_expression(Expression::Kind::Variable, offset);
        expr->name = identifier();
        if (accept("(")) {
            expr->kind = Expression::Kind::Call;
            if (!accept(")")) {
                do {
                    expr->arguments.push_back(expression());
                } while (accept(","));
                require(")");
            }
        }
        return expr;
    }

    bool accept(std::string_view text) {
        if (token_.text != text) return false;
        advance();
        return true;
    }

    void require(std::string_view text) {
        if (!accept(text)) fail("expected '" + std::string(text) + "'");
    }

    std::string identifier() {
        if (token_.kind != Kind::Identifier || reserved(token_.text)) {
            fail("expected non-keyword identifier");
        }
        std::string result(token_.text);
        advance();
        return result;
    }

    static void validate_expression(const Expression& expr,
                                    const std::unordered_set<std::string>& parameters,
                                    const std::unordered_map<std::string, std::size_t>& functions) {
        switch (expr.kind) {
            case Expression::Kind::Integer:
            case Expression::Kind::Boolean: return;
            case Expression::Kind::Variable:
                if (parameters.count(expr.name) == 0) {
                    throw std::runtime_error("unknown parameter '" + expr.name
                                             + "' at byte " + std::to_string(expr.offset));
                }
                return;
            case Expression::Kind::Call: {
                const auto found = functions.find(expr.name);
                if (found == functions.end() || found->second != expr.arguments.size()) {
                    throw std::runtime_error("unknown function or wrong call arity '"
                                             + expr.name + "' at byte "
                                             + std::to_string(expr.offset));
                }
                for (const auto& arg : expr.arguments) {
                    validate_expression(*arg, parameters, functions);
                }
                return;
            }
            case Expression::Kind::Negate:
                validate_expression(*expr.left, parameters, functions);
                return;
            case Expression::Kind::Add:
            case Expression::Kind::Subtract:
            case Expression::Kind::Multiply:
                validate_expression(*expr.left, parameters, functions);
                validate_expression(*expr.right, parameters, functions);
                return;
        }
        throw std::runtime_error("internal invalid expression kind");
    }

    static std::string shape_key(const MessageShape& shape) {
        switch (shape.kind) {
            case MessageShape::Kind::Boolean: return "bool";
            case MessageShape::Kind::Integer: return "int";
            case MessageShape::Kind::String: return "string";
            default: break;
        }
        const char* label = shape.kind == MessageShape::Kind::Tuple ? "Tuple"
            : shape.kind == MessageShape::Kind::FixedArray ? "FixedArray"
            : shape.kind == MessageShape::Kind::FixedList ? "FixedList"
            : shape.kind == MessageShape::Kind::Array ? "Array" : "List";
        std::string text(label);
        text += "[";
        for (const auto& element : shape.elements) {
            if (text.back() != '[') text += ",";
            text += shape_key(element);
        }
        return text + "]";
    }

    static bool contains_call(const Expression& e) {
        if (e.kind == Expression::Kind::Call) return true;
        if (e.left && contains_call(*e.left)) return true;
        if (e.right && contains_call(*e.right)) return true;
        for (const auto& arg : e.arguments)
            if (contains_call(*arg)) return true;
        return false;
    }

    static bool contains_boolean(const Expression& e) {
        if (e.kind == Expression::Kind::Boolean) return true;
        if (e.left && contains_boolean(*e.left)) return true;
        if (e.right && contains_boolean(*e.right)) return true;
        for (const auto& arg : e.arguments)
            if (contains_boolean(*arg)) return true;
        return false;
    }

    static bool reads_name(const Expression& e, const std::string& name) {
        if (e.kind == Expression::Kind::Variable && e.name == name) return true;
        if (e.left && reads_name(*e.left, name)) return true;
        if (e.right && reads_name(*e.right, name)) return true;
        for (const auto& arg : e.arguments)
            if (reads_name(*arg, name)) return true;
        return false;
    }

    static bool reads_state_value(const Expression& e) {
        if (e.kind == Expression::Kind::Variable
                && e.name.rfind("@state.", 0) == 0) return true;
        if (e.left && reads_state_value(*e.left)) return true;
        if (e.right && reads_state_value(*e.right)) return true;
        for (const auto& arg : e.arguments)
            if (reads_state_value(*arg)) return true;
        return false;
    }

    static void validate(const Program& program) {
        // LLVM nominal declarations must resolve independently of source
        // order, but must NEVER inherit structural shape-first dispatch.
        std::unordered_set<std::string> declaredNames;
        std::unordered_map<std::string, const EnumDeclaration*> enumNames;
        for (const auto& enumeration : program.enums) {
            if (!declaredNames.insert(enumeration.name).second)
                throw std::runtime_error("duplicate enum/pattern declaration");
            enumNames.emplace(enumeration.name, &enumeration);
        }
        std::unordered_set<std::string> typedDomains;
        for (const auto& pattern : program.tagged_patterns) {
            if (!declaredNames.insert(pattern.name).second)
                throw std::runtime_error("duplicate enum/pattern declaration");
            typedDomains.insert(pattern.name);
            for (const auto& qualified : pattern.alternatives) {
                const std::size_t dot = qualified.find('.');
                auto found = enumNames.find(qualified.substr(0, dot));
                if (found == enumNames.end())
                    throw std::runtime_error("unknown enum in tagged pattern: " + qualified);
                const std::string name = qualified.substr(dot + 1);
                if (std::none_of(found->second->cases.begin(), found->second->cases.end(),
                                 [&](const EnumCase& c) { return c.name == name; }))
                    throw std::runtime_error("unknown enum case in tagged pattern: " + qualified);
            }
        }
        for (const auto& [name, ignored] : enumNames) typedDomains.insert(name);
        std::unordered_map<std::string, const MessagePattern*> messagePatterns;
        for (const auto& pattern : program.message_patterns) {
            if (!messagePatterns.emplace(pattern.name, &pattern).second)
                throw std::runtime_error("duplicate message match contract '" + pattern.name + "'");
            std::unordered_set<std::string> variants;
            for (const auto& alternative : pattern.alternatives) {
                if (!variants.insert(shape_key(alternative)).second)
                    throw std::runtime_error("duplicate message shape in match '" + pattern.name + "'");
            }
        }
        for (const auto& item : program.message_patterns) {
            if (!declaredNames.insert(item.name).second)
                throw std::runtime_error("duplicate structural/tagged message contract");
        }
        std::unordered_set<std::string> declaredActors;
        for (const auto& actor : program.actors) declaredActors.insert(actor.name);
        // A route intrinsic is admitted only inside a source-defined actor
        // with a compiler-checked, owned pipeline role in trusted main.
        std::unordered_map<std::string, unsigned> pipelineRoles;
        // A routed nominal case must belong to the actual destination's
        // declared ingress domain, not merely the emitting actor's domain.
        // Keys are actor types; unique types per linear pipeline are required.
        std::unordered_map<std::string,
            std::pair<std::string, std::string>> pipelineNeighbors;
        std::unordered_map<std::string, std::size_t> functions;
        for (const auto& fn : program.functions) {
            if (!functions.emplace(fn.name, fn.parameters.size()).second) {
                throw std::runtime_error("duplicate function '" + fn.name + "' at byte "
                                         + std::to_string(fn.offset));
            }
            std::unordered_set<std::string> params;
            for (const auto& param : fn.parameters) {
                if (!params.insert(param).second) {
                    throw std::runtime_error("duplicate parameter '" + param
                                             + "' at byte " + std::to_string(fn.offset));
                }
            }
        }
        for (const auto& fn : program.functions) {
            std::unordered_set<std::string> available(fn.parameters.begin(),
                                                      fn.parameters.end());
            if (!fn.actor_spawns.empty() || !fn.actor_stops.empty()
                || !fn.ready_await_actor.empty()) {
                if (fn.name != "main" || !fn.parameters.empty())
                    throw std::runtime_error("source actor spawn requires zero-argument main");
            }
            std::unordered_set<std::string> liveActors;
            std::unordered_map<std::string, std::string> actorTypes;
            for (const auto& spawn : fn.actor_spawns) {
                if (!declaredActors.count(spawn.actor_type))
                    throw std::runtime_error("unknown actor type '" + spawn.actor_type + "'");
                if (!liveActors.insert(spawn.name).second ||
                    !available.insert(spawn.name).second)
                    throw std::runtime_error("duplicate actor reference '" + spawn.name + "'");
                actorTypes.emplace(spawn.name, spawn.actor_type);
            }
            if (!fn.ready_await_actor.empty()) {
                // First source continuation prototype intentionally supports
                // a single linear ActorRef, no scalar locals across yield,
                // and deterministic trusted-main teardown after resumption.
                if (fn.actor_spawns.size() != 1 || !fn.locals.empty() ||
                    !liveActors.count(fn.ready_await_actor))
                    throw std::runtime_error(
                        "await get_ready requires one source actor without suspended locals");
            }
            if (!fn.pipeline_actors.empty()) {
                if (fn.name != "main" || !fn.parameters.empty()
                    || fn.pipeline_actors.size() < 2
                    || fn.pipeline_actors.size() > 16 || !fn.ready_await_actor.empty())
                    throw std::runtime_error("pipeline requires 2..16 actors in zero-argument main without ready await");
                std::unordered_set<std::string> unique;
                std::unordered_set<std::string> uniqueTypes;
                std::string contract;
                for (const auto& refName : fn.pipeline_actors) {
                    if (!unique.insert(refName).second || !actorTypes.count(refName))
                        throw std::runtime_error("pipeline actor ref missing or repeated");
                    const auto found = std::find_if(program.actors.begin(), program.actors.end(),
                        [&](const Actor& a) { return a.name == actorTypes.at(refName); });
                    if (found == program.actors.end()
                        || (found->transform_pipe
                            ? (!typedDomains.count(found->forward_messages_type)
                               || !typedDomains.count(found->backward_messages_type))
                            : !typedDomains.count(found->messages_type)))
                        throw std::runtime_error("pipeline actors require nominal typed mailbox");
                    if (!uniqueTypes.insert(found->name).second)
                        throw std::runtime_error("linear pipeline requires distinct actor types");
                    pipelineRoles.emplace(found->name,
                        (unsigned)(&refName - &fn.pipeline_actors.front()));
                    // Directional transform domains are resolved to a shared
                    // pipeline wire table by the native backend. Ordinary
                    // legacy pipelines continue to require one domain.
                    if (!found->transform_pipe) {
                        if (contract.empty()) contract = found->messages_type;
                        else if (contract != found->messages_type
                                 && std::none_of(program.actors.begin(), program.actors.end(),
                                        [](const Actor& a) { return a.transform_pipe; }))
                            throw std::runtime_error("pipeline actor message domains must match");
                    }
                }
                // A transform needs upstream forward ingress and downstream
                // backward ingress. Root external DATA is direction 0 and
                // cannot safely be dispatched to a transform hook.
                const auto& firstType = actorTypes.at(fn.pipeline_actors.front());
                const auto& lastType = actorTypes.at(fn.pipeline_actors.back());
                for (const auto& actor : program.actors)
                    if ((actor.name == firstType || actor.name == lastType)
                            && actor.transform_pipe)
                        throw std::runtime_error(
                            "transform pipe actor must be an interior pipeline stage");
                for (std::size_t i = 0; i < fn.pipeline_actors.size(); ++i) {
                    const auto& type = actorTypes.at(fn.pipeline_actors[i]);
                    const auto upstream = i ? actorTypes.at(fn.pipeline_actors[i-1]) : "";
                    const auto downstream = i+1 < fn.pipeline_actors.size()
                        ? actorTypes.at(fn.pipeline_actors[i+1]) : "";
                    if (!pipelineNeighbors.emplace(type,
                            std::make_pair(upstream, downstream)).second)
                        throw std::runtime_error(
                            "actor type cannot belong to multiple pipeline routes");
                }
                if (fn.pipeline_expect_actor != fn.pipeline_actors.front())
                    throw std::runtime_error("expect_pipeline must name pipeline source");
                if (!fn.pipeline_cancel_actor.empty()
                    && fn.pipeline_cancel_actor != fn.pipeline_actors.front())
                    throw std::runtime_error("cancel_pipeline must name pipeline source");
                if (fn.actor_sends.size() != 1
                    || fn.actor_sends.front().reference != fn.pipeline_actors.front()
                    || fn.actor_stops.size() != fn.pipeline_actors.size())
                    throw std::runtime_error("pipeline requires one source send and a stop for each actor");
                for (const auto& refName : fn.pipeline_actors)
                    if (std::count(fn.actor_stops.begin(), fn.actor_stops.end(), refName) != 1)
                        throw std::runtime_error("pipeline must stop each connected actor");
            } else if (!fn.pipeline_expect_actor.empty()
                       || !fn.pipeline_cancel_actor.empty()) {
                throw std::runtime_error("expect_pipeline/cancel_pipeline requires declared pipeline");
            }
            // Actor references cannot be cast to i64 or used as scalar values.
            for (const auto& actor : liveActors) available.erase(actor);
            for (const auto& local : fn.locals) {
                if (liveActors.count(local.name))
                    throw std::runtime_error("scalar local shadows actor reference '" + local.name + "'");
                // The initializer is checked BEFORE publication: x=x+1 or
                // forward references cannot read uninitialized stack memory.
                validate_expression(*local.initializer, available, functions);
                if (!available.insert(local.name).second) {
                    throw std::runtime_error("duplicate owned local '" + local.name
                                             + "' at byte " + std::to_string(local.offset));
                }
            }
            for (const auto& send : fn.actor_sends) {
                auto ref = actorTypes.find(send.reference);
                if (ref == actorTypes.end())
                    throw std::runtime_error("unknown actor message receiver '" + send.reference + "'");
                const auto actor = std::find_if(program.actors.begin(), program.actors.end(),
                    [&](const Actor& a) { return a.name == ref->second; });
                if (actor == program.actors.end()) throw std::runtime_error("unknown actor type");
                if (!actor->messages_type.empty()) {
                    if (typedDomains.count(actor->messages_type)) {
                        if (send.tagged_variant.empty())
                            throw std::runtime_error("tagged actor send requires Enum.Case");
                        std::unordered_set<std::string> allowed;
                        for (const auto& pattern : program.tagged_patterns)
                            if (pattern.name == actor->messages_type)
                                allowed.insert(pattern.alternatives.begin(),
                                               pattern.alternatives.end());
                        if (const auto own = enumNames.find(actor->messages_type);
                                own != enumNames.end())
                            for (const auto& c : own->second->cases)
                                allowed.insert(own->first + "." + c.name);
                        if (!allowed.count(send.tagged_variant))
                            throw std::runtime_error("actor send case outside closed tagged pattern");
                        const auto dot = send.tagged_variant.find('.');
                        const auto declared = enumNames.at(send.tagged_variant.substr(0, dot));
                        const auto member = std::find_if(declared->cases.begin(),
                            declared->cases.end(), [&](const EnumCase& c) {
                                return c.name == send.tagged_variant.substr(dot + 1);
                            });
                        if (member == declared->cases.end())
                            throw std::runtime_error("unknown tagged variant");
                        if (!member->has_payload) {
                            if (send.payload)
                                throw std::runtime_error("payloadless enum send has arguments");
                        } else if (member->payload.kind != MessageShape::Kind::Integer
                                   || !send.payload
                                   || (send.payload->kind != Expression::Kind::Integer
                                       && !(send.payload->kind == Expression::Kind::Negate
                                            && send.payload->left
                                            && send.payload->left->kind == Expression::Kind::Integer))) {
                            // This compiler subset still requires a compile-time
                            // signed-i64 literal. Do not silently lower general
                            // expressions or truncate to a 32-bit enum tag.
                            throw std::runtime_error(
                                "LLVM tagged int send requires a signed int literal");
                        }
                        continue;
                    }
                    if (!send.tagged_variant.empty())
                        throw std::runtime_error("nominal case send requires tagged actor");
                    auto found = messagePatterns.find(actor->messages_type);
                    if (found == messagePatterns.end())
                        throw std::runtime_error("unknown actor message match contract");
                    const MessagePattern& contract = *found->second;
                    if (contract.alternatives.size() != 1 ||
                        (contract.alternatives.front().kind != MessageShape::Kind::Boolean
                         && contract.alternatives.front().kind != MessageShape::Kind::Integer))
                        throw std::runtime_error(
                            "typed structural/union message transport requires tagged envelope lowering");
                    const bool expectedBool =
                        contract.alternatives.front().kind == MessageShape::Kind::Boolean;
                    if (expectedBool && send.payload->kind != Expression::Kind::Boolean)
                        throw std::runtime_error("actor send expects bool message");
                    if (!expectedBool && contains_boolean(*send.payload))
                        throw std::runtime_error("actor send expects int message");
                } else {
                    if (!send.tagged_variant.empty())
                        throw std::runtime_error("nominal case send requires tagged actor");
                    const auto method = std::find_if(actor->methods.begin(), actor->methods.end(),
                        [](const Function& fn) { return fn.name == "on_message"; });
                    if (method == actor->methods.end() || !method->exported ||
                        method->parameters.size() != 1)
                        throw std::runtime_error("actor send requires pub on_message(int)");
                }
                validate_expression(*send.payload, available, functions);
            }
            validate_expression(*fn.result, available, functions);
            for (const auto& name : fn.actor_stops) {
                if (!liveActors.erase(name))
                    throw std::runtime_error("invalid or repeated actor stop '" + name + "'");
            }
            if (!liveActors.empty())
                throw std::runtime_error("actor reference must be stopped before return");
        }

        std::unordered_set<std::string> actorNames;
        std::unordered_set<std::string> generatedSymbols;
        auto reserveGenerated = [&](const std::string& name) {
            if (functions.count(name) || !generatedSymbols.insert(name).second)
                throw std::runtime_error("conflicting generated actor symbol '" + name + "'");
        };
        for (const auto& actor : program.actors) {
            if (!actor.messages_type.empty() || actor.transform_pipe) {
                if (actor.transform_pipe
                    && (!typedDomains.count(actor.forward_messages_type)
                        || !typedDomains.count(actor.backward_messages_type)))
                    throw std::runtime_error("unknown transform pipe message domain");
                if (!actor.transform_pipe && !messagePatterns.count(actor.messages_type)
                        && !typedDomains.count(actor.messages_type))
                    throw std::runtime_error(
                        "unknown actor message match contract '" + actor.messages_type + "'");
                const auto count = std::count_if(actor.methods.begin(), actor.methods.end(),
                    [](const Function& f) { return f.is_receiver; });
                if (actor.transform_pipe) {
                    const auto forward = std::count_if(actor.methods.begin(), actor.methods.end(),
                        [](const Function& f) { return f.name == "receive_forward"; });
                    const auto backward = std::count_if(actor.methods.begin(), actor.methods.end(),
                        [](const Function& f) { return f.name == "receive_backward"; });
                    if (count != 2 || forward != 1 || backward != 1)
                        throw std::runtime_error("transform pipe actor requires exactly one receive_forward and receive_backward");
                } else if (count != 1)
                    throw std::runtime_error("typed actor requires exactly one receive with its declared message domain");
                if (std::any_of(actor.methods.begin(), actor.methods.end(),
                    [](const Function& f) { return f.name == "on_message"; }))
                    throw std::runtime_error(
                        "typed actor must use receive, not on_message");
            } else if (std::any_of(actor.methods.begin(), actor.methods.end(),
                    [](const Function& f) { return f.is_receiver; })) {
                throw std::runtime_error("receive requires actor messages=Contract");
            }
            if (!actorNames.insert(actor.name).second
                || functions.count(actor.name) != 0) {
                throw std::runtime_error("duplicate actor symbol '" + actor.name + "'");
            }
            const std::string prefix = "oreslang_actor_"
                + (program.module.empty() ? "" : program.module + "_")
                + actor.name + "_";
            reserveGenerated(prefix + "init");
            if (actor.transform_pipe) reserveGenerated(prefix + "receive");
            std::unordered_set<std::string> fields;
            std::unordered_set<std::string> mutableFields;
            // Deliberately restrict initializers to closed arithmetic:
            // initialization must not escape the actor turn or call FFI.
            const std::unordered_set<std::string> none;
            const std::unordered_map<std::string, std::size_t> noCalls;
            for (const auto& field : actor.fields) {
                validate_expression(*field.initializer, none, noCalls);
                if (field.name == "state" || field.name == "using"
                        || field.name == "read" || field.name == "write")
                    throw std::runtime_error("actor state key conflicts with reserved capability name");
                if (!fields.insert(field.name).second)
                    throw std::runtime_error(
                        std::string(actor.state_declared
                            ? "duplicate actor state key '" : "duplicate actor field '")
                        + field.name + "'");
                if (field.field_mutable) mutableFields.insert(field.name);
            }
            std::unordered_set<std::string> methods;
            for (const auto& method : actor.methods) {
                // Neither ordinary actor methods nor the legacy boolean
                // receive matcher may touch actor state outside a scoped
                // handle_state callback. Reject before the LLVM lowering
                // attempts to resolve state as an ordinary class local.
                if (!method.assignments.empty()
                    || std::any_of(method.boolean_arms.begin(),
                        method.boolean_arms.end(),
                        [](const ReceiveBooleanArm& arm) {
                            return !arm.assignments.empty();
                        }))
                    throw std::runtime_error(
                        "actor state mutation requires scoped handle_state callback");
                if (!methods.insert(method.name).second)
                    throw std::runtime_error("duplicate actor method '" + method.name + "'");
                reserveGenerated(prefix + method.name);
                // Actor class fields are never ordinary lexical values.
                // The only legal field reads/writes happen under a state
                // loan with explicit compiler-owned @state.key bindings.
                std::unordered_set<std::string> available;
                for (const auto& param : method.parameters) {
                    if (!available.insert(param).second)
                        throw std::runtime_error("actor parameter shadows field '" + param + "'");
                }
                for (const auto& local : method.locals) {
                    validate_expression(*local.initializer, available, functions);
                    if (!available.insert(local.name).second)
                        throw std::runtime_error("actor local shadows field/parameter '" + local.name + "'");
                }
                if (method.is_receiver) {
                    const std::string methodDomain = actor.transform_pipe
                        ? (method.name == "receive_forward"
                            ? actor.forward_messages_type : actor.backward_messages_type)
                        : actor.messages_type;
                    const std::string& parameter = method.parameters.at(0);
                    if (!method.boolean_arms.empty()) {
                        if (typedDomains.count(methodDomain))
                            throw std::runtime_error("boolean match cannot inspect tagged enum");
                        const MessagePattern& contract =
                            *messagePatterns.at(methodDomain);
                        if (contract.alternatives.size() != 1 ||
                            contract.alternatives[0].kind != MessageShape::Kind::Boolean)
                            throw std::runtime_error(
                                "receive match true/false requires bool-only contract");
                        if (method.match_subject != parameter)
                            throw std::runtime_error(
                                "receive match must inspect Message parameter");
                        bool seenTrue = false, seenFalse = false;
                        for (const auto& arm : method.boolean_arms) {
                            bool& seen = arm.match_value ? seenTrue : seenFalse;
                            if (seen) throw std::runtime_error(
                                "duplicate receive boolean match arm");
                            seen = true;
                            for (const auto& assignment : arm.assignments) {
                                if (!mutableFields.count(assignment.field))
                                    throw std::runtime_error(
                                        "cannot assign immutable or unknown actor field in match");
                                if (reads_name(*assignment.value, parameter))
                                    throw std::runtime_error(
                                        "match arm Message payload binding is not narrowed");
                                validate_expression(*assignment.value, available, functions);
                            }
                            if (arm.state_loan) {
                                const auto& loan = *arm.state_loan;
                                if (!actor.state_declared)
                                    throw std::runtime_error("handle_state requires an actor state map");
                                if (!method.effects_declared)
                                    throw std::runtime_error("actor state method requires explicit with effects");
                                const auto permits = [&](std::string_view effect) {
                                    return std::find(method.declared_effects.begin(),
                                        method.declared_effects.end(), effect)
                                            != method.declared_effects.end();
                                };
                                if (loan.mode == StateLoan::Mode::Read && !permits("state.read"))
                                    throw std::runtime_error("state.read loan not permitted by with effects");
                                if (loan.mode == StateLoan::Mode::Write && !permits("state.write"))
                                    throw std::runtime_error("state.write loan not permitted by with effects");
                                bool reads = reads_state_value(*loan.result);
                                for (const auto& assignment : loan.assignments)
                                    reads |= reads_state_value(*assignment.value);
                                if (reads && !permits("state.read"))
                                    throw std::runtime_error("state.read operation not permitted by with effects");
                                if (loan.mode == StateLoan::Mode::Read && !loan.assignments.empty())
                                    throw std::runtime_error("read-only actor state loan cannot mutate state");
                                auto loanAvailable = available;
                                for (const auto& field : actor.fields)
                                    loanAvailable.insert("@state." + field.name);
                                for (const auto& assignment : loan.assignments) {
                                    if (!mutableFields.count(assignment.field))
                                        throw std::runtime_error("state callback writes unknown or immutable key");
                                    if (reads_name(*assignment.value, parameter))
                                        throw std::runtime_error("match arm Message payload binding is not narrowed");
                                    if (contains_call(*assignment.value))
                                        throw std::runtime_error("state read/write loan cannot invoke arbitrary functions");
                                    validate_expression(*assignment.value, loanAvailable, functions);
                                }
                                if (reads_name(*loan.result, parameter))
                                    throw std::runtime_error("match arm Message payload binding is not narrowed");
                                if (contains_call(*loan.result))
                                    throw std::runtime_error("state read/write loan cannot invoke arbitrary functions");
                                validate_expression(*loan.result, loanAvailable, functions);
                                if (available.count(loan.result_binding)
                                        || fields.count(loan.result_binding))
                                    throw std::runtime_error("state callback result shadows actor state or method local");
                            }
                        }
                        if (!seenTrue || !seenFalse)
                            throw std::runtime_error(
                                "nonexhaustive receive boolean match");
                    }
                    if (!method.tagged_arms.empty()) {
                        if (!typedDomains.count(methodDomain))
                            throw std::runtime_error(
                                "tagged receive match requires named enum or pattern");
                        if (method.match_subject != parameter)
                            throw std::runtime_error(
                                "tagged receive match must inspect Message parameter");
                        std::unordered_set<std::string> allowed;
                        for (const auto& pattern : program.tagged_patterns) {
                            if (pattern.name == methodDomain)
                                allowed.insert(pattern.alternatives.begin(),
                                               pattern.alternatives.end());
                        }
                        const auto directEnum = enumNames.find(methodDomain);
                        if (directEnum != enumNames.end()) {
                            for (const auto& c : directEnum->second->cases)
                                allowed.insert(directEnum->first + "." + c.name);
                        }
                        std::unordered_set<std::string> seen;
                        for (const auto& arm : method.tagged_arms) {
                            if (!allowed.count(arm.qualified_case))
                                throw std::runtime_error(
                                    "receive match arm is outside closed tagged pattern");
                            if (!seen.insert(arm.qualified_case).second)
                                throw std::runtime_error(
                                    "duplicate or unreachable tagged receive match arm");
                            const auto dot = arm.qualified_case.find('.');
                            const auto enumName = arm.qualified_case.substr(0, dot);
                            const auto variant = arm.qualified_case.substr(dot + 1);
                            const auto enumeration = enumNames.at(enumName);
                            const auto matched = std::find_if(enumeration->cases.begin(),
                                enumeration->cases.end(),
                                [&](const EnumCase& c) { return c.name == variant; });
                            if (matched == enumeration->cases.end())
                                throw std::runtime_error("unresolved tagged enum case");
                            auto armAvailable = available;
                            for (const auto& binder : arm.binders) {
                                if (binder != "_") {
                                    if (!armAvailable.insert(binder).second)
                                        throw std::runtime_error("tagged binder shadows parameter or local");
                                }
                            }
                            if (arm.state_loan) {
                                if (method.effects_declared) {
                                    const auto permits = [&](std::string_view effect) {
                                        return std::find(method.declared_effects.begin(),
                                            method.declared_effects.end(), effect)
                                                != method.declared_effects.end();
                                    };
                                    if (arm.state_loan->mode == StateLoan::Mode::Read
                                            && !permits("state.read"))
                                        throw std::runtime_error(
                                            "state.read loan not permitted by with effects");
                                    if (arm.state_loan->mode == StateLoan::Mode::Write
                                            && !permits("state.write"))
                                        throw std::runtime_error(
                                            "state.write loan not permitted by with effects");
                                    bool reads = reads_state_value(*arm.state_loan->result);
                                    for (const auto& assignment : arm.state_loan->assignments)
                                        reads |= reads_state_value(*assignment.value);
                                    if (reads && !permits("state.read"))
                                        throw std::runtime_error(
                                            "state.read operation not permitted by with effects");
                                }
                                if (!actor.state_declared)
                                    throw std::runtime_error("handle_state requires an actor state map");
                                if (arm.state_loan->mode == StateLoan::Mode::Read
                                        && !arm.state_loan->assignments.empty())
                                    throw std::runtime_error(
                                        "read-only actor state loan cannot mutate state");
                                std::unordered_set<std::string> loanAvailable = armAvailable;
                                for (const auto& field : actor.fields)
                                    loanAvailable.insert("@state." + field.name);
                                for (const auto& assignment : arm.state_loan->assignments) {
                                    if (!mutableFields.count(assignment.field))
                                        throw std::runtime_error(
                                            "state callback writes unknown or immutable key");
                                    if (contains_call(*assignment.value))
                                        throw std::runtime_error(
                                            "state read/write loan cannot invoke arbitrary functions");
                                    validate_expression(*assignment.value, loanAvailable, functions);
                                }
                                if (contains_call(*arm.state_loan->result))
                                    throw std::runtime_error(
                                        "state read/write loan cannot invoke arbitrary functions");
                                validate_expression(*arm.state_loan->result,
                                                    loanAvailable, functions);
                                if (fields.count(arm.state_loan->result_binding)
                                        || !armAvailable.insert(arm.state_loan->result_binding).second)
                                    throw std::runtime_error(
                                        "state callback result shadows actor state or arm local");
                            }
                            for (const auto& assignment : arm.assignments) {
                                if (!mutableFields.count(assignment.field))
                                    throw std::runtime_error(
                                        "cannot assign immutable or unknown actor field in tagged match");
                                if (reads_name(*assignment.value, parameter))
                                    throw std::runtime_error(
                                        "tagged match arm Message payload binding is not narrowed");
                                validate_expression(*assignment.value, armAvailable, functions);
                            }
                            if (!arm.route_direction.empty()) {
                                if (actor.transform_pipe
                                    && ((method.name == "receive_forward" && arm.route_direction != "forward")
                                        || (method.name == "receive_backward" && arm.route_direction != "backward")))
                                    throw std::runtime_error("transform handler must emit in its ingress direction");
                                const auto role = pipelineRoles.find(actor.name);
                                if (role == pipelineRoles.end())
                                    throw std::runtime_error("pipeline actor routing requires declared pipeline topology");
                                if ((role->second == 0 && arm.route_direction == "backward")
                                    || (role->second == pipelineRoles.size() - 1
                                        && arm.route_direction == "forward"))
                                    throw std::runtime_error("pipeline route direction is not authorized for actor role");
                                if (!arm.route_payload || arm.tuple_destructure)
                                    throw std::runtime_error("pipeline route requires scalar payload expression");
                                if (arm.route_variant.find('.') == std::string::npos
                                        || !allowed.count(arm.route_variant))
                                    throw std::runtime_error("pipeline route outside nominal message contract");
                                const auto neighbor = pipelineNeighbors.find(actor.name);
                                if (neighbor == pipelineNeighbors.end())
                                    throw std::runtime_error("pipeline route lacks a registered neighbor");
                                const std::string& targetName =
                                    arm.route_direction == "forward"
                                        ? neighbor->second.second : neighbor->second.first;
                                const auto receiver = std::find_if(
                                    program.actors.begin(), program.actors.end(),
                                    [&](const Actor& other) { return other.name == targetName; });
                                if (targetName.empty() || receiver == program.actors.end())
                                    throw std::runtime_error("pipeline route has no target actor");
                                const std::string recipientDomain = !receiver->transform_pipe
                                    ? receiver->messages_type
                                    : arm.route_direction == "forward"
                                        ? receiver->forward_messages_type
                                        : receiver->backward_messages_type;
                                bool recipientAccepts = false;
                                for (const auto& pattern : program.tagged_patterns)
                                    if (pattern.name == recipientDomain
                                        && std::find(pattern.alternatives.begin(),
                                                      pattern.alternatives.end(),
                                                      arm.route_variant) != pattern.alternatives.end())
                                        recipientAccepts = true;
                                if (const auto en = enumNames.find(recipientDomain);
                                        en != enumNames.end()) {
                                    const auto& prefix = recipientDomain + ".";
                                    if (arm.route_variant.rfind(prefix, 0) == 0)
                                        recipientAccepts = std::any_of(
                                            en->second->cases.begin(), en->second->cases.end(),
                                            [&](const EnumCase& c) {
                                                return arm.route_variant == prefix + c.name;
                                            });
                                }
                                if (!recipientAccepts)
                                    throw std::runtime_error(
                                        "pipeline destination ingress domain rejects routed case");
                                const auto output = enumNames.at(arm.route_variant.substr(0,
                                    arm.route_variant.find('.')));
                                const auto outputCase = std::find_if(output->cases.begin(),
                                    output->cases.end(), [&](const EnumCase& c) {
                                        return c.name == arm.route_variant.substr(
                                            arm.route_variant.find('.') + 1); });
                                if (outputCase == output->cases.end() || !outputCase->has_payload
                                    || outputCase->payload.kind != MessageShape::Kind::Integer)
                                    throw std::runtime_error("pipeline route requires typed int enum case");
                                validate_expression(*arm.route_payload, armAvailable, functions);
                            }
                            if (arm.binds_payload) {
                                if (!matched->has_payload)
                                    throw std::runtime_error(
                                        "cannot destructure payloadless enum variant");
                                const auto kind = matched->payload.kind;
                                const bool fixed = kind == MessageShape::Kind::Tuple
                                    || kind == MessageShape::Kind::FixedArray
                                    || kind == MessageShape::Kind::FixedList;
                                const std::size_t expected = fixed
                                    ? matched->payload.elements.size() : 1;
                                if (arm.binders.size() != expected
                                    || (fixed != arm.tuple_destructure))
                                    throw std::runtime_error(
                                        "tagged variant binder arity or tuple shape mismatch");
                                std::unordered_set<std::string> names;
                                for (const auto& name : arm.binders) {
                                    // '_' is an intentionally ignored slot,
                                    // not a binding. Multiple ignored tuple
                                    // positions are valid and never shadow.
                                    if (name == "_") continue;
                                    if (!names.insert(name).second)
                                        throw std::runtime_error(
                                            "duplicate variable in tagged payload destructuring");
                                    if (available.count(name))
                                        throw std::runtime_error(
                                            "tagged payload binder shadows field, local or Message parameter");
                                }
                            }
                        }
                        if (seen.size() != allowed.size())
                            throw std::runtime_error("nonexhaustive tagged receive match");
                    }
                    if (reads_name(*method.result, parameter) ||
                        std::any_of(method.assignments.begin(), method.assignments.end(),
                            [&](const FieldAssignment& a) {
                                return reads_name(*a.value, parameter);
                            }))
                        throw std::runtime_error(
                            "receive Message payload inspection requires match lowering");
                }
                for (const auto& assignment : method.assignments) {
                    if (!mutableFields.count(assignment.field)) {
                        throw std::runtime_error("cannot assign immutable or unknown actor field '" + assignment.field + "'");
                    }
                    validate_expression(*assignment.value, available, functions);
                }
                if (method.method_state_loan) {
                    // Ordinary methods require explicit effect bounds; no
                    // implicit escalation via compiler inference.
                    const auto& loan = *method.method_state_loan;
                    if (!actor.state_declared)
                        throw std::runtime_error("handle_state requires an actor state map");
                    if (!method.effects_declared)
                        throw std::runtime_error("actor state method requires explicit with effects");
                    const auto permits = [&](std::string_view effect) {
                        return std::find(method.declared_effects.begin(),
                            method.declared_effects.end(), effect)
                                != method.declared_effects.end();
                    };
                    if (loan.mode == StateLoan::Mode::Read && !permits("state.read"))
                        throw std::runtime_error("state.read loan not permitted by with effects");
                    if (loan.mode == StateLoan::Mode::Write && !permits("state.write"))
                        throw std::runtime_error("state.write loan not permitted by with effects");
                    bool reads = reads_state_value(*loan.result);
                    for (const auto& assignment : loan.assignments)
                        reads |= reads_state_value(*assignment.value);
                    if (reads && !permits("state.read"))
                        throw std::runtime_error("state.read operation not permitted by with effects");
                    if (loan.mode == StateLoan::Mode::Read && !loan.assignments.empty())
                        throw std::runtime_error("read-only actor state loan cannot mutate state");
                    auto loanAvailable = available;
                    for (const auto& field : actor.fields)
                        loanAvailable.insert("@state." + field.name);
                    for (const auto& assignment : loan.assignments) {
                        if (!mutableFields.count(assignment.field))
                            throw std::runtime_error("state callback writes unknown or immutable key");
                        if (contains_call(*assignment.value))
                            throw std::runtime_error("state read/write loan cannot invoke arbitrary functions");
                        validate_expression(*assignment.value, loanAvailable, functions);
                    }
                    if (contains_call(*loan.result))
                        throw std::runtime_error("state read/write loan cannot invoke arbitrary functions");
                    validate_expression(*loan.result, loanAvailable, functions);
                    if (!available.insert(loan.result_binding).second
                            || fields.count(loan.result_binding))
                        throw std::runtime_error("state callback result shadows actor state or method local");
                }
                // A copied scalar may be returned from a tagged match
                // only when every arm defines the same callback result.
                // This is a definite-assignment check, not a state loan
                // escaping its critical callback.
                auto returnAvailable = available;
                if (!method.boolean_arms.empty()) {
                    const auto& firstLoan = method.boolean_arms.front().state_loan;
                    if (firstLoan && std::all_of(method.boolean_arms.begin(),
                            method.boolean_arms.end(), [&](const ReceiveBooleanArm& arm) {
                                return arm.state_loan
                                    && arm.state_loan->result_binding == firstLoan->result_binding;
                            }))
                        returnAvailable.insert(firstLoan->result_binding);
                }
                if (!method.tagged_arms.empty()) {
                    const auto& firstLoan = method.tagged_arms.front().state_loan;
                    if (firstLoan && std::all_of(method.tagged_arms.begin(),
                            method.tagged_arms.end(), [&](const TaggedReceiveArm& arm) {
                                return arm.state_loan
                                    && arm.state_loan->result_binding
                                        == firstLoan->result_binding;
                            }))
                        returnAvailable.insert(firstLoan->result_binding);
                }
                validate_expression(*method.result, returnAvailable, functions);
            }
            // Static variant coverage alone cannot authorize execution:
            // per-case MessagePack DATA and typed continuation lowering
            // must be implemented before guest source reaches LLVM IR.
            if (typedDomains.count(actor.messages_type) || actor.transform_pipe) {
                // Exhaustiveness is directional for transform pipe actors:
                // each handler matches exactly its declared input domain.
                for (const auto& receiver : actor.methods) {
                    if (!receiver.is_receiver) continue;
                    const std::string domain = !actor.transform_pipe
                        ? actor.messages_type
                        : receiver.name == "receive_forward"
                            ? actor.forward_messages_type : actor.backward_messages_type;
                    std::vector<std::string> allowed;
                    for (const auto& pattern : program.tagged_patterns)
                        if (pattern.name == domain) allowed = pattern.alternatives;
                    if (const auto found = enumNames.find(domain);
                            found != enumNames.end())
                        for (const auto& c : found->second->cases)
                            allowed.push_back(found->first + "." + c.name);
                    if (allowed.empty() || allowed.size() > 64)
                        throw std::runtime_error("LLVM tagged actor supports 1..64 variants per direction");
                    for (const auto& alternative : allowed) {
                        const auto dot = alternative.find('.');
                        const auto owner = enumNames.at(alternative.substr(0, dot));
                        const auto it = std::find_if(owner->cases.begin(), owner->cases.end(),
                            [&](const EnumCase& c) {
                                return c.name == alternative.substr(dot + 1);
                            });
                        if (it == owner->cases.end()
                                || (it->has_payload
                                    && it->payload.kind != MessageShape::Kind::Integer))
                            throw std::runtime_error(
                                "LLVM tagged source subset requires payloadless or int cases");
                    }
                    if (receiver.tagged_arms.size() != allowed.size()
                            || !receiver.boolean_arms.empty()
                            || !receiver.assignments.empty())
                        throw std::runtime_error(
                            "LLVM tagged actor requires exhaustive side-effect-only match");
                }
            }
        }
    }

    void advance() { token_ = lexer_.next(); }
    [[noreturn]] void fail(const std::string& reason) const {
        throw std::runtime_error(reason + " at byte " + std::to_string(token_.offset));
    }

    Lexer lexer_;
    Token token_;
    unsigned state_loan_depth_ = 0;
};

} // namespace

Program parse_program(std::string_view source) {
    return Parser(source).parse();
}

} // namespace oreslang
