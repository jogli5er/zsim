#ifndef _CL_BM_HELPER_
#define _CL_BM_HELPER_ 1
#include <stdlib.h>
#include <stdint.h>
#include <vector>
uint64_t
set_accessed(uint64_t mask, uint8_t lower, uint8_t upper);
uint64_t
get_total_mask_for_presence(const std::vector<uint64_t> &masks);

uint8_t
get_total_access_from_masks(const std::vector<uint64_t> &masks);

std::vector<std::pair<uint8_t, uint8_t>>
get_start_end_of_bitmask(const uint64_t &mask);

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
count_holes_in_masks(const uint64_t &mask);
#endif