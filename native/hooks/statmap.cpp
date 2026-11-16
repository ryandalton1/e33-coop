#include "statmap.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>

#include <Unreal/UObject.hpp>

#include "ue.hpp"

using namespace RC::Unreal;

namespace e33::statmap {
namespace {

// Memory layout of TMap<uint8 enum, double> (UE5): TSet<TPair<K, V>> over a TSparseArray whose first member is the
// element TArray {Data*, Num, Max}. Element = TSetElement { TPair{uint8 key; (pad); double value}; int32 HashNextId;
// int32 HashIndex } = 24 bytes. Stat maps are filled once per battle and never shrink, so every slot below Num is
// allocated; values are checked anyway (key range, finite value) before any read or write.
struct Element {
    uint8_t key;
    uint8_t pad[7];
    double value;
    int32_t hash_next;
    int32_t hash_index;
};
static_assert(sizeof(Element) == 24);

struct ScriptArray {
    Element* data;
    int32_t num;
    int32_t max;
};

constexpr int kMaxKey = 32;
constexpr int kMaxEntries = 64;

Element* find(UObject* stats, const wchar_t* map, int stat)
{
    if (!stats) return nullptr;
    auto* arr = ue::value_ptr<ScriptArray>(stats, map);
    if (!arr || !arr->data || arr->num <= 0 || arr->num > kMaxEntries || arr->num > arr->max) return nullptr;
    for (int i = 0; i < arr->num; ++i) {
        Element& e = arr->data[i];
        if (e.key > kMaxKey) return nullptr;     // not the layout we expect: touch nothing
        if (e.key == stat) return std::isfinite(e.value) ? &e : nullptr;
    }
    return nullptr;
}

} // namespace

std::optional<double> get(UObject* stats, const wchar_t* map, int stat)
{
    if (Element* e = find(stats, map, stat)) return e->value;
    return std::nullopt;
}

bool set(UObject* stats, const wchar_t* map, int stat, double value)
{
    if (!std::isfinite(value)) return false;
    Element* e = find(stats, map, stat);
    if (!e) return false;
    e->value = value;
    return true;
}

double max_hp(UObject* stats)
{
    if (!stats) return 0;
    ue::Call c(stats, STR("GetMaxHP"));
    if (c && c.invoke()) {
        const double v = c.out_double(STR("MaxHP"), c.out_double(STR("ReturnValue"), 0));
        if (v > 0) return v;
    }
    return get(stats, STR("CharacterCurrentStats"), kHP).value_or(0);
}

bool set_max_hp(UObject* stats, double value)
{
    set(stats, STR("CharacterBaseStats"), kHP, value);
    return set(stats, STR("CharacterCurrentStats"), kHP, value);
}

} // namespace e33::statmap
