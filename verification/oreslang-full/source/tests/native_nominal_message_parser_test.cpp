#include "oreslang/frontend/Parser.hpp"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

static void rejects(std::string_view source, const std::string& phrase) {
    try {
        (void)oreslang::parse_program(source);
    } catch (const std::runtime_error& error) {
        if (std::string(error.what()).find(phrase) != std::string::npos) return;
        std::cerr << "incorrect diagnostic: " << error.what()
                  << " (wanted " << phrase << ")\n";
        std::abort();
    }
    std::cerr << "invalid nominal message source unexpectedly accepted\n";
    std::abort();
}
int main() {
    using namespace oreslang;
    auto p = parse_program(R"(
        define pattern Commands as
            Events.Compute
            Events.Ping
            Events.Stop
        end
        define enum Events as
            Compute: Tuple[string, int]
            Ping: string = 3
            Stop = 10
        end
    )");
    assert(p.enums.size() == 1 && p.tagged_patterns.size() == 1);
    assert(p.enums.at(0).name == "Events");
    assert(p.enums.at(0).cases.size() == 3);
    assert(p.enums.at(0).cases.at(0).tag == 1);
    assert(p.enums.at(0).cases.at(1).tag == 3);
    assert(p.enums.at(0).cases.at(2).tag == 10);
    assert(!p.enums.at(0).cases.at(2).has_payload);
    assert(p.enums.at(0).cases.at(0).payload.kind == MessageShape::Kind::Tuple);
    assert(p.enums.at(0).cases.at(0).payload.elements.size() == 2);
    assert(p.tagged_patterns.at(0).alternatives.at(1) == "Events.Ping");

    auto collection = parse_program(R"(
        define enum Other as
            Many: List[int]
            Fixed: FixedArray[string, string, string]
            More: Array[bool]
            Items: FixedList[bool, int]
        end
        define pattern All as
            Other.Many, Other.Fixed;
            Other.More
            Other.Items
        end
    )");
    assert(collection.enums.size() == 1);
    assert(collection.enums.at(0).cases.at(1).payload.kind == MessageShape::Kind::FixedArray);
    assert(collection.enums.at(0).cases.at(3).payload.kind == MessageShape::Kind::FixedList);
    assert(collection.tagged_patterns.at(0).alternatives.size() == 4);

    rejects("define enum Bad as A: string = 2 B: string = 1 end",
            "enum wire tags must increase monotonically");
    rejects("define enum Bad as A: string = 0 end",
            "enum wire tag must be a positive");
    rejects("define enum Bad as A: string = 2147483648 end",
            "enum wire tag must be a positive");
    rejects("define enum Bad as A B A end", "duplicate enum case");
    rejects("define enum Bad as A: List[bool, int] end",
            "variable enum message collection must have exactly one element type");
    rejects("define enum Bad as A: Tuple[int, List[bool, bool]] end",
            "variable enum message collection must have exactly one element type");
    rejects("define enum Bad as A: FixedArray[List[int, int], string] end",
            "variable enum message collection must have exactly one element type");
    rejects("define enum Bad as A: Array[Tuple[string, Array[int, bool]]] end",
            "variable enum message collection must have exactly one element type");
    // One bad descriptor can be partitioned into individually legal
    // 4096-slot tuples. Quota the TOTAL tree rather than each subtree.
    std::string huge = "define enum Huge as Payload: Tuple[";
    for (int group = 0; group < 26; ++group) {
        if (group) huge += ",";
        huge += "Tuple[";
        for (int n = 0; n < 4096; ++n) {
            if (n) huge += ",";
            huge += "int";
        }
        huge += "]";
    }
    huge += "] end";
    rejects(huge, "message shape exceeds total descriptor node budget");
    rejects("define pattern Missing as Events.Nope end",
            "unknown enum in tagged pattern");
    rejects("define enum E as A end define pattern P as E.Missing end",
            "unknown enum case in tagged pattern");
    rejects("define enum E as A end define pattern P as E.A E.A end",
            "duplicate pattern alternative");
    rejects("define enum E as A end define pattern E as E.A end",
            "duplicate enum/pattern declaration");
    rejects(R"(
        define enum E as A: string end
        define pattern P as E.A end
        define actor Worker<messages=P> as
            receive(Message m) { }
        end
    )", "LLVM tagged source subset requires payloadless or int cases");

    // Static compiler admission proves exact coverage, source-order
    // alternatives and binders before intentionally rejecting execution.
    const std::string taggedActor = R"(
        define enum E as
            Compute: Tuple[string, int] = 1
            Ping: string = 2
            Stop = 3
        end
        define pattern Messages as E.Compute E.Ping E.Stop end
        define actor Worker<messages=Messages> as
            receive(Message m) {
                do match m over {
                    on E.Compute as (text, n) {}
                    on E.Ping as label {}
                    on E.Stop {}
                }
            }
        end
    )";
    rejects(taggedActor,
        "LLVM tagged source subset requires payloadless or int cases");
    const auto arm = std::string("on E.Compute as (text, n) {}");
    rejects(std::string(taggedActor).replace(taggedActor.find("on E.Stop {}"),
            std::string("on E.Stop {}").size(), ""),
            "nonexhaustive tagged receive match");
    rejects(std::string(taggedActor).replace(taggedActor.find("on E.Stop {}"),
            std::string("on E.Stop {}").size(), "on E.Ping {}"),
            "duplicate or unreachable tagged receive match arm");
    rejects(std::string(taggedActor).replace(taggedActor.find("on E.Stop {}"),
            std::string("on E.Stop {}").size(), "on E.NotListed {}"),
            "receive match arm is outside closed tagged pattern");
    rejects(std::string(taggedActor).replace(taggedActor.find(arm), arm.size(),
            "on E.Compute as (text) {}"),
            "tagged variant binder arity or tuple shape mismatch");
    rejects(std::string(taggedActor).replace(taggedActor.find("on E.Stop {}"),
            std::string("on E.Stop {}").size(), "on E.Stop as x {}"),
            "cannot destructure payloadless enum variant");
    rejects(std::string(taggedActor).replace(taggedActor.find("on E.Compute as (text, n) {}"),
            arm.size(), "on E.Compute as (x, x) {}"),
            "duplicate variable in tagged payload destructuring");
    // '_' is not a bound variable. Repeated ignored slots preserve tuple
    // shape without introducing duplicate bindings or shadowing.
    rejects(std::string(taggedActor).replace(taggedActor.find(arm), arm.size(),
            "on E.Compute as (_, _) {}"),
            "LLVM tagged source subset requires payloadless or int cases");
    rejects(std::string(taggedActor).replace(taggedActor.find("on E.Ping as label {}"),
            std::string("on E.Ping as label {}").size(), "on E.Ping as m {}"),
            "tagged payload binder shadows field, local or Message parameter");
    rejects(std::string(taggedActor).replace(taggedActor.find("receive(Message m)"),
            std::string("receive(Message m)").size(),
            "let count: int = 0; receive(Message m)").replace(
            taggedActor.find("on E.Ping as label {}")
                + std::string("let count: int = 0; ").size(),
            std::string("on E.Ping as label {}").size(), "on E.Ping as count {}"),
            "tagged payload binder shadows field, local or Message parameter");
    rejects(std::string(taggedActor).replace(taggedActor.find("match m over"),
            std::string("match m over").size(), "match unknown over"),
            "tagged receive match must inspect Message parameter");
    rejects(std::string(taggedActor).replace(taggedActor.find("on E.Stop {}"),
            std::string("on E.Stop {}").size(), "on E.Stop { self.respond(1); }"),
            "unsupported self pipeline operation");

    // The executable, deliberately narrow LLVM source subset now admits
    // payloadless tagged sends, exhaustive arms and scalar actor state.
    auto executable = parse_program(R"(
        define enum Signals as Ping = 1 Pong = 255 Stop = 65536 end
        define pattern WorkerMessages as Signals.Pong Signals.Ping Signals.Stop end
        define actor Worker<messages=WorkerMessages> as
            let count: int = 0;
            receive(Message m) {
                do match m over {
                    on Signals.Ping { count = count + 4; }
                    on Signals.Pong { count = count + 2; }
                    on Signals.Stop { count = count + 8; }
                }
                return count;
            }
        end
        pub fnc main(): int {
            const worker = spawn Worker();
            worker.send(Signals.Pong);
            worker.send(Signals.Ping);
            worker.send(Signals.Stop);
            stop worker;
            return 43;
        }
    )");
    assert(executable.actors.size() == 1 && executable.functions.size() == 1);
    assert(executable.functions.at(0).actor_sends.size() == 3);
    assert(executable.functions.at(0).actor_sends.at(0).tagged_variant == "Signals.Pong");
    assert(!executable.functions.at(0).actor_sends.at(0).payload);
    assert(executable.actors.at(0).methods.at(0).tagged_arms.size() == 3);
    assert(executable.actors.at(0).methods.at(0).tagged_arms.at(0).assignments.size() == 1);
    // Typed source must fail closed if a case is outside the closed pattern.
    rejects(R"(
        define enum Signals as Ping Stop end
        define pattern OnlyPing as Signals.Ping end
        define actor Worker<messages=OnlyPing> as
            receive(Message m) {
                do match m over { on Signals.Ping {} }
            }
        end
        pub fnc main(): int {
            const worker = spawn Worker();
            worker.send(Signals.Stop);
            stop worker;
            return 43;
        }
    )", "actor send case outside closed tagged pattern");
    rejects(R"(
        define enum Signals as Ping Stop end
        define pattern Both as Signals.Ping Signals.Stop end
        define actor Worker<messages=Both> as
            receive(Message m) { }
        end
    )", "LLVM tagged actor requires exhaustive side-effect-only match");

    // Compiler-executable typechecked scalar payload variant: binder is
    // scoped to its own exhaustive receive arm and cannot leak elsewhere.
    auto typedInt = parse_program(R"(
        define enum Values as Increment: int = 4 Decrement: int = 7 end
        define pattern Inbox as Values.Decrement Values.Increment end
        define actor Counter<messages=Inbox> as
            let count: int = 0;
            receive(Message m) {
                do match m over {
                    on Values.Increment as n { count = count + n; }
                    on Values.Decrement as n { count = count - n; }
                }
                return count;
            }
        end
        pub fnc main(): int {
            const worker = spawn Counter();
            worker.send(Values.Increment(9));
            worker.send(Values.Decrement(3));
            stop worker;
            return 43;
        }
    )");
    assert(typedInt.actors.size() == 1);
    assert(typedInt.functions.at(0).actor_sends.size() == 2);
    assert(typedInt.functions.at(0).actor_sends.at(0).payload->integer == 9);
    assert(typedInt.actors.at(0).methods.at(0).tagged_arms.at(0).binders.at(0) == "n");
    rejects(R"(
        define enum Values as Increment: int end
        define actor Counter<messages=Values> as
            receive(Message m) { do match m over { on Values.Increment as n {} } }
        end
        pub fnc main(): int {
            const worker = spawn Counter();
            worker.send(Values.Increment(true));
            stop worker;
            return 43;
        }
    )", "LLVM tagged int send requires a signed int literal");
    rejects(R"(
        define enum Values as Increment: int end
        define actor Counter<messages=Values> as
            receive(Message m) { do match m over { on Values.Increment as n {} } }
        end
        pub fnc main(): int {
            const worker = spawn Counter();
            worker.send(Values.Increment);
            stop worker;
            return 43;
        }
    )", "LLVM tagged int send requires a signed int literal");

    // Existing bool-only actor receive semantics remain usable.
    auto legacy = parse_program(R"(
        declare type match Signals as bool end
        define actor Worker<messages=Signals> as
            let count: int = 0;
            receive(Message m) {
                match m {
                    true => { count = count + 1; }
                    false => { count = count + 2; }
                }
                return count;
            }
        end
    )");
    assert(legacy.message_patterns.size() == 1);
    assert(legacy.actors.size() == 1);
    std::cout << "NOMINAL_MESSAGE_PARSER_PASS enums=1 cases=3 tagged_patterns=1 "
                 "legacy_bool=1 fail_closed_guest=1\n";
}
