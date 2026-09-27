#include "awl/world_map_actor_action.h"

#include <array>

namespace awl {

namespace {

struct Row {
    uint8_t weight;
    int32_t action;
    int32_t parameter;
};

using Table = std::array<Row, 8>;

// Eight distinct 8-row patterns from the verified DOL. Several branch
// addresses contain byte-identical copies; retain the actual address below.
constexpr Table k5595C{{
    {10, 0, 0}, {25, 1, 0}, {15, 3, 0}, {15, 6, 1},
    {20, 7, 1}, {15, 8, 1}, {0, 10, 0}, {0, 10, 0}}};
constexpr Table k55ADC{{
    {10, 0, 0}, {15, 1, 0}, {15, 3, 0}, {20, 5, 1},
    {15, 6, 0}, {15, 9, 0}, {10, 10, 0}, {0, 13, 0}}};
constexpr Table k55B3C{{
    {0, 0, 0}, {0, 1, 0}, {0, 3, 0}, {0, 5, 1},
    {0, 6, 0}, {30, 9, 0}, {70, 10, 0}, {0, 13, 0}}};
constexpr Table k55B9C{{
    {35, 0, 0}, {15, 1, 0}, {15, 3, 0}, {15, 5, 1},
    {10, 6, 0}, {10, 9, 0}, {0, 10, 0}, {0, 13, 0}}};
constexpr Table k55D7C{{
    {10, 0, 0}, {25, 1, 0}, {15, 2, 0}, {15, 3, 1},
    {20, 4, 1}, {15, 5, 1}, {0, 7, 0}, {0, 7, 0}}};
constexpr Table k55E9C{{
    {50, 0, 0}, {50, 1, 1}, {0, 2, 1}, {0, 3, 1},
    {0, 4, 1}, {0, 6, 0}, {0, 6, 0}, {0, 6, 0}}};
constexpr Table k55FBC{{
    {10, 0, 0}, {35, 1, 0}, {10, 3, 1}, {15, 4, 1},
    {15, 5, 1}, {0, 7, 0}, {15, 8, 0}, {0, 10, 0}}};
constexpr Table k5613C{{
    {20, 0, 0}, {45, 1, 0}, {10, 3, 1}, {10, 4, 1},
    {5, 5, 1}, {0, 7, 0}, {10, 8, 0}, {0, 10, 0}}};

const Table* pattern_for(uint32_t address) {
    switch (address) {
    case 0x8025595c: case 0x802559bc: return &k5595C;
    case 0x80255adc: case 0x80255c5c: return &k55ADC;
    case 0x80255b3c: case 0x80255cbc: return &k55B3C;
    case 0x80255b9c: case 0x80255bfc: return &k55B9C;
    case 0x80255d7c: case 0x80255ddc: case 0x80255e3c: return &k55D7C;
    case 0x80255e9c: case 0x80255efc: case 0x80255f5c: return &k55E9C;
    case 0x80255fbc: case 0x802561fc: return &k55FBC;
    case 0x8025613c: case 0x8025619c: return &k5613C;
    default: return nullptr;
    }
}

uint8_t animation_group(int32_t variant, int32_t action) {
    if (variant == 3) {
        return action == 10 ? 4 : (action == 11 ? 5 : 0);
    }
    return variant == 4 && action == 1 ? 4 : 0;
}

} // namespace

bool choose_world_map_first_actor_action(
    const WorldMapFirstActorTargetDecision& selection,
    int32_t current_action, int32_t previous_action,
    std::optional<uint32_t> table_rng_word,
    std::optional<uint32_t> override_rng_word,
    WorldMapFirstActorActionChoice* choice) {
    if (choice == nullptr || !selection.selector_ran ||
        selection.handler == WorldMapFirstActorHandler::None ||
        selection.state.actor_id < 0x2c || selection.state.actor_id > 0x30) {
        return false;
    }
    const int32_t variant = selection.state.actor_id - 0x2c;
    WorldMapFirstActorActionChoice next;
    uint32_t table_address = 0;
    const auto route_b460 = [&]() {
        next.route = WorldMapFirstActorActionRoute::Call8015B460;
    };
    const auto route_bda8 = [&]() {
        next.route = WorldMapFirstActorActionRoute::Call8015BDA8;
    };

    if (selection.handler == WorldMapFirstActorHandler::Idle) {
        constexpr uint32_t tables[]{
            0x80255fbc, 0x80255d7c, 0x8025595c,
            0x80255adc, 0x80255e9c};
        table_address = tables[variant];
    } else {
        if (!selection.handler_score || selection.handler_target_index < 0) {
            return false;
        }
        // Both score thresholds at r2-0x66D0/-0x66CF are zero in this DOL.
        const bool positive_score = *selection.handler_score > 0;
        if (selection.handler == WorldMapFirstActorHandler::FirstTarget) {
            switch (variant) {
            case 0: route_bda8(); break;
            case 1: if (positive_score) table_address = 0x80255ddc;
                    else route_b460(); break;
            case 2: if (positive_score) table_address = 0x802559bc;
                    else route_b460(); break;
            case 3: table_address = current_action == 10 ? 0x80255b3c :
                        (positive_score ? 0x80255c5c : 0x80255b9c); break;
            case 4: table_address = 0x80255efc; break;
            }
        } else if (selection.handler == WorldMapFirstActorHandler::SecondTarget) {
            switch (variant) {
            case 0: table_address = positive_score ? 0x802561fc : 0x8025613c;
                    break;
            case 1: if (positive_score) table_address = 0x80255e3c;
                    else route_bda8(); break;
            case 2: if (positive_score) table_address = 0x802559bc;
                    else route_bda8(); break;
            case 3: if (current_action == 10) table_address = 0x80255cbc;
                    else if (positive_score) table_address = 0x80255c5c;
                    else route_b460(); break;
            case 4: table_address = 0x80255f5c; break;
            }
        } else {
            return false;
        }
    }
    if (next.route != WorldMapFirstActorActionRoute::WeightedTable) {
        *choice = next;
        return true;
    }
    const Table* table = pattern_for(table_address);
    if (table == nullptr || !table_rng_word) {
        return false;
    }
    const uint32_t draw = *table_rng_word % 100u;
    uint32_t cumulative = 0;
    bool selected = false;
    for (size_t row = 0; row < table->size(); ++row) {
        cumulative = (cumulative + (*table)[row].weight) & 0xffu;
        if (draw <= cumulative) {
            next.table_address = table_address;
            next.table_row = static_cast<uint8_t>(row);
            next.action_code = (*table)[row].action;
            next.action_parameter = (*table)[row].parameter;
            selected = true;
            break;
        }
    }
    if (!selected) {
        return false;
    }
    if (selection.handler == WorldMapFirstActorHandler::Idle &&
        variant == 3 && previous_action == 3) {
        if (!override_rng_word) {
            return false;
        }
        if (*override_rng_word % 100u < 50u) {
            next.action_code = 11;
            next.action_parameter = 1;
        }
    }
    next.animation_group = animation_group(variant, *next.action_code);
    *choice = next;
    return true;
}

} // namespace awl
