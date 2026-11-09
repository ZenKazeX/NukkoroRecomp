// Original exception-runtime oracle for the native type-query failure boundary.
// liveness-roots: reference-interpreter-only
#include "support/type_unwind_oracle.h"

using namespace type_unwind_oracle;

int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected original DOL path");
        Oracle oracle(argv[1]);
        unsigned pairs = 0;
        for (bool caught : {false, true}) for (unsigned shape = 0; shape < 4; ++shape)
            for (u32 flag : {1u, 0x8000u, 0xFFFF0001u}) for (unsigned seed : {0u, 1u, 19u}) {
                const auto original = oracle.run(false, caught, shape, flag, seed);
                const auto recovered = oracle.run(true, caught, shape, flag, seed);
                require(equal(original, recovered), "recovered failure boundary changed actual exception unwinding");
                require(original.catches == unsigned(caught) && original.callbacks == unsigned(caught) &&
                        original.cleanup_value == unsigned(caught) && original.throws == (caught ? 2u : 1u),
                        "catch, cleanup or rethrow coverage was missed");
                ++pairs;
            }
        const auto correct = oracle.run(false, true, 3, 1, 0);
        const auto broken = oracle.run(true, true, 3, 1, 0, true);
        require(!equal(correct, broken) && broken.catches == 0,
                "mutation gate failed to detect a wrong unwind return PC");
        unsigned typed_pairs = 0;
        for (const auto& item : std::array<std::pair<const char*, u32>, 5>{{
                 {"!std::bad_cast!", 1}, {"!std::exception!", 1}, {"!Other!", 0}, {"Pv", 0}, {nullptr, 1}}}) {
            const auto original = oracle.match_exception_type(item.first, false);
            const auto recovered = oracle.match_exception_type(item.first, true);
            require(original == recovered && original.first == item.second && original.second == 0,
                    "removing class metadata changed typed exception matching");
            ++typed_pairs;
        }
        std::printf("PASS type-query unwind: %u original/recovered pairs; actual catch, cleanup and rethrow; "
                    "cast instructions erased in recovered fixtures; wrong-return-PC mutation rejected; "
                    "%u typed matcher pairs; %zu class-metadata bytes erased; %zu native header identities; "
                    "%llu original instructions. Generated failure entry, no production integration.\n",
                    pairs, typed_pairs, oracle.metadata_size, oracle.headers.size(),
                    static_cast<unsigned long long>(oracle.instructions));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL type-query unwind: %s\n", error.what());
        return 1;
    }
}
