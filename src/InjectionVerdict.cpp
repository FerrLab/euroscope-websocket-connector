#include "InjectionVerdict.h"

InjectionVerdict JudgeInjection(const std::string& injected,
                                const std::string& current)
{
    if (current == injected)
        return InjectionVerdict::NotConsumed;
    if (current.empty())
        return InjectionVerdict::Consumed;
    return InjectionVerdict::Overwritten;
}
