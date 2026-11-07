// -*- C++ -*-
// ============================================================================
// module: deren.vulkan.core_support
//
// The backend-detail templates that are tested in isolation. They are a
// standalone module rather than core partitions because the tests that cover
// them do not link `deren_vulkan` and must not import a backend module just to
// reach a header-sized utility.
// ============================================================================
export module deren.vulkan.core_support;

export import :lazy_loading;
export import :push_range_validation;
