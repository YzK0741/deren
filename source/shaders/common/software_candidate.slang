#ifndef DEREN_SOFTWARE_CANDIDATE_SLANG
#define DEREN_SOFTWARE_CANDIDATE_SLANG
bool software_candidate_usable(uint id, uint bits) {
    float depth = asfloat(bits);
    return id != 0u && isfinite(depth) && depth >= 0.0 && depth <= 1.0;
}
// Called before depth merge; retain the winning candidate's own reconstruction depth.
bool choose_visibility_candidate(inout uint hardware_id, inout float hardware_depth, uint id, uint bits) {
    if (!software_candidate_usable(id, bits)) return false;
    const float depth = asfloat(bits);
    if (depth < hardware_depth || (depth == hardware_depth && (hardware_id & 0x80000000u) == 0u)) {
        hardware_id = id;
        hardware_depth = depth;
        return true;
    }
    return false;
}
#endif
