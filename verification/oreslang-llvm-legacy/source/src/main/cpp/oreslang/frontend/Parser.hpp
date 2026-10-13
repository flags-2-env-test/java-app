#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace oreslang {

// Deliberately restricted, typed-i64 expression AST. Not the full Java Ast.Program.
struct Expression {
    enum class Kind { Integer, Boolean, Variable, Negate, Add, Subtract, Multiply, Call };
    Kind kind;
    std::size_t offset = 0;
    std::int64_t integer = 0;
    std::string name;
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;
    std::vector<std::unique_ptr<Expression>> arguments;
};

// A trivially owned, immutable i64 local. The first LLVM lexical-storage
// lowering scope has deterministic entry/exit, with no heap or GC collector.
struct LocalBinding {
    std::string name;
    std::unique_ptr<Expression> initializer;
    bool heap_owned = false;
    bool field_mutable = false; // Only `let` actor fields; locals remain immutable.
    std::size_t offset = 0;
};

// Distinguish source i64 data, generation-fenced actor capabilities, and
// suspended results in the AST. Future lowering remains a compiler gate.
struct SourceType {
    enum class Kind { I64, ActorRef, Future };
    Kind kind = Kind::I64;
    std::string argument;
};

struct ActorSpawnBinding {
    std::string name;
    std::string actor_type;
    SourceType declared_type;
    std::size_t offset = 0;
};

struct ActorSend {
    std::string reference;
    std::unique_ptr<Expression> payload;
    // Nominal case, e.g. Messages.Stop; never lowered as scalar i64.
    std::string tagged_variant;
    std::size_t offset = 0;
};

struct FieldAssignment {
    std::string field;
    std::unique_ptr<Expression> value;
    std::size_t offset = 0;
};

struct ReceiveBooleanArm {
    bool match_value = false;
    std::vector<FieldAssignment> assignments;
    std::size_t offset = 0;
};

// Static admission representation for source `match m over { on Enum.Case
// as (...) { } }`. Bodies remain unavailable to native codegen; matching
// contracts are checked before the explicit fail-closed lowering gate.
// Fixed-shape state is visible only during this scoped, synchronous
// callback. The callback returns a copied i64 scalar, never a state loan.
struct StateLoan {
    enum class Mode { Read, Write };
    Mode mode = Mode::Write;
    std::string result_binding;
    std::vector<FieldAssignment> assignments;
    std::unique_ptr<Expression> result;
};

struct TaggedReceiveArm {
    std::unique_ptr<StateLoan> state_loan;
    std::string qualified_case;
    // One terminal directional send after any field assignments; lowered
    // into a scheduler-admitted message, never a synchronous actor call.
    std::string route_direction;
    std::string route_variant;
    std::unique_ptr<Expression> route_payload;
    std::vector<std::string> binders;
    bool binds_payload = false;
    bool tuple_destructure = false;
    std::vector<FieldAssignment> assignments;
    std::size_t offset = 0;
};

struct Function {
    std::string name;
    std::vector<std::string> parameters;
    bool effects_declared = false;
    std::vector<std::string> declared_effects; // restricted state.read / state.write row
    std::vector<LocalBinding> locals;
    std::vector<ActorSpawnBinding> actor_spawns;
    std::vector<ActorSend> actor_sends;
    std::vector<std::string> pipeline_actors; // compiler-owned 2..16 ordered refs
    std::string pipeline_cancel_actor;  // trusted main, before any send
    std::string pipeline_expect_actor;
    std::int64_t pipeline_expect_value = 0;
    // Restricted initial compiler continuation: await ref.get_ready()
    // directly after spawning actors, before messages or cleanup.
    std::string ready_await_actor;
    std::vector<std::string> actor_stops;
    std::vector<FieldAssignment> assignments;
    std::string match_subject;
    std::vector<ReceiveBooleanArm> boolean_arms;
    std::vector<TaggedReceiveArm> tagged_arms;
    std::unique_ptr<Expression> result;
    std::size_t offset = 0;
    bool exported = false;
    bool is_receiver = false;
};

// Closed structural variants, not arbitrary runtime object references.
// Tuple/Array/List alternatives retain ordered element types and arity.
struct MessageShape {
    enum class Kind { Boolean, Integer, String, Tuple, Array, List, FixedArray, FixedList };
    Kind kind;
    std::vector<MessageShape> elements;
};

struct MessagePattern {
    std::string name;
    std::vector<MessageShape> alternatives;
    std::size_t offset = 0;
};

// Parsed nominal tagged contracts. These are compile-time declarations only;
// until LLVM's full typed DATA lowering is implemented they are deliberately
// rejected when used as an actor mailbox instead of silently using scalar ABI.
struct EnumCase {
    std::string name;
    MessageShape payload;
    bool has_payload = false;
    std::uint32_t tag = 0;
};
struct EnumDeclaration {
    std::string name;
    std::vector<EnumCase> cases;
    std::size_t offset = 0;
};
struct TaggedPattern {
    std::string name;
    std::vector<std::string> alternatives;
    std::size_t offset = 0;
};

struct Actor {
    std::string name;
    std::string messages_type;
    // Explicit two-direction actor role. Ordinary actors retain messages_type.
    bool transform_pipe = false;
    std::string forward_messages_type;
    std::string backward_messages_type;
    bool state_declared = false;
    std::vector<LocalBinding> fields; // compiler-owned fixed-key state map
    std::vector<Function> methods;
    std::size_t offset = 0;
};

struct Program {
    std::string module;
    std::vector<Function> functions;
    std::vector<Actor> actors;
    std::vector<MessagePattern> message_patterns;
    std::vector<EnumDeclaration> enums;
    std::vector<TaggedPattern> tagged_patterns;
};

// Parse and statically validate a strict Java-compatible subset: int functions,
// int parameters, return expressions and same-module calls. Reject everything else.
Program parse_program(std::string_view source);

} // namespace oreslang
