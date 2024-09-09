#ifndef SCOPE
#define SCOPE

#include <concepts>
#include <functional>
#include <type_traits>

#define fwd(...) static_cast<decltype(__VA_ARGS__) &&>(__VA_ARGS__)

template<std::invocable Function>
class ScopeGaurd {
public:
    template<typename... CArgs>
        requires std::constructible_from<Function, CArgs...>
    constexpr ScopeGaurd(CArgs &&...cargs) : m_Func{fwd(cargs)...} {}

    ScopeGaurd(ScopeGaurd const &) = delete;

    ScopeGaurd &operator=(ScopeGaurd const &) = delete;

    constexpr ~ScopeGaurd() { m_Func(); }

private:
    Function m_Func;
};

template<typename Func>
ScopeGaurd(Func) -> ScopeGaurd<Func>;

constexpr auto scopeAction(std::invocable auto &&scopeStartAction, std::invocable auto &&scopeEndAction) {
    if constexpr (std::is_void_v<std::invoke_result_t<decltype(scopeStartAction)>>) {
        std::invoke(fwd(scopeStartAction));
        return ScopeGaurd{fwd(scopeEndAction)};
    } else
        return ScopeGaurd{[result = std::invoke(fwd(scopeStartAction)), seAction = fwd(scopeEndAction)]() mutable {
            std::invoke(mov(seAction), mov(result));
        }};
}

#endif
