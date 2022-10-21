#include "cacheline_access_bitmask_helpers.h"
#include <iostream>
uint64_t
set_accessed(uint64_t mask, uint8_t lower, uint8_t upper)
{
    if (upper > 64) {
        upper = 64;
    }

    uint64_t bitmask = 1;
    for (uint8_t i = 0; i < lower; i++) {
        bitmask = bitmask << 1;
    }

    for (uint8_t i = lower; i < upper; i++) {
        mask |= bitmask;
        bitmask = bitmask << 1;
    }
    return mask;
}

uint64_t
get_total_mask_for_presence(const std::vector<uint64_t> &masks)
{
    uint64_t current_mask = 0;
    for (auto const &mask : masks) {
        current_mask |= mask;
    }
    return current_mask;
}

std::vector<std::pair<uint8_t, uint8_t>>
get_start_end_of_bitmask(const uint64_t &mask)
{
    std::vector<std::pair<uint8_t, uint8_t>> result;
    uint8_t prev_bit = 0;
    uint8_t current_bit = 0;
    bool trailing = true;
    std::pair<uint8_t, uint8_t> current_block;
    for (int byte = 0; byte < 64; byte++) {
        current_bit = ((mask >> byte) & 0x1);
        if (current_bit == 1 && trailing) {
            trailing = false;
            prev_bit = current_bit;
            current_block.first = byte;
            continue;
        } else if (current_bit == 0 && trailing) {
            continue;
        }

        // posedge/negedge kind of analysis
        if (current_bit == 0 && prev_bit == 1) {
            current_block.second = byte - 1;
            result.push_back(current_block);
            current_block = std::pair<uint8_t, uint8_t>();
        } else if (current_bit == 1 && prev_bit == 0) {
            current_block.first = byte;
        }
        prev_bit = current_bit;
    }

    if (prev_bit == 1 && current_bit == 1) {
        current_block.second = 63;
        result.push_back(current_block);
    }
    return result;
}

uint8_t
get_total_access_from_masks(const std::vector<uint64_t> &masks)
{
    return (uint8_t)__builtin_popcountll(get_total_mask_for_presence(masks));
}

/**
 * @brief Parses a bit mask to detect holes and blocks in a cacheline
 *
 * @param the mask of the cacheline under test
 * @return std::vector<int> A vector containing B H B H B sizes (Block and hole sizes),
 * where it alwazs needs to return an odd number of entries, as it alwazs has to start
 * and end with a block and in between tw blcks there is always exclusively one single
 * hole.
 */
std::vector<int>
count_holes_in_masks(const uint64_t &mask)
{
    std::vector<int> holes;
    auto blocks = get_start_end_of_bitmask(mask);
    std::pair<int8_t, int8_t> prev_block;
    for (auto block : blocks) {
        if (holes.size() > 0) {
            // hole
            holes.push_back(block.first - prev_block.second);
        }
        // block
        holes.push_back(block.second - block.first + 1);

        prev_block = block;
    }

    if (holes.size() % 2 != 1) {
        std::cout << "WHAT IS WRONG WITH YOU???" << std::endl;
    }
    return holes;
}
