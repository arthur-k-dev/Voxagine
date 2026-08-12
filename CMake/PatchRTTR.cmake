# Fixes one upstream bug in RTTR 0.9.6, applied as FetchContent's PATCH_COMMAND.
#
# Run as: cmake -DRTTR_SOURCE_DIR=<dir> -P CMake/PatchRTTR.cmake
#
# `type_register_private::register_base_class_info` sorts a type's base classes
# with `is_base_of` as the comparator:
#
#     std::sort(..., [](const base_class_info& l, const base_class_info& r)
#                    { return l.m_base_type.is_base_of(r.m_base_type); });
#
# That is not a strict weak ordering, so it is undefined behaviour rather than
# merely a questionable order. Strict weak ordering requires that "neither
# precedes the other" be *transitive*, and for `is_base_of` it is not: given
# unrelated A and B, and B unrelated to C, A may still be a base of C. std::sort
# is entitled to run off the end of the sequence when its comparator lies, and
# on the iOS Debug build it does - the app aborts inside this sort during static
# initialisation, before main() is reached, while registering BehaviorScript's
# bases. The same UB has been latent on desktop for years; the ordering the
# comparator happens to produce there stays in bounds.
#
# The replacement orders by base-class count, which is a real strict weak
# ordering (it compares integers) and preserves what the comment above the call
# asks for: a base always has strictly fewer bases than anything derived from
# it, so roots still sort first. stable_sort keeps declaration order among types
# with equal depth, which is the RTTR_ENABLE(CLASS) order the comment relies on -
# plain sort left that to chance.
#
# Idempotent: patching an already-patched tree is a no-op, so it is safe on the
# re-runs CMake's download step performs.

if(NOT RTTR_SOURCE_DIR)
    message(FATAL_ERROR "PatchRTTR.cmake needs -DRTTR_SOURCE_DIR")
endif()

set(RTTR_TYPE_REGISTER "${RTTR_SOURCE_DIR}/src/rttr/detail/type/type_register.cpp")

if(NOT EXISTS "${RTTR_TYPE_REGISTER}")
    message(FATAL_ERROR "PatchRTTR: ${RTTR_TYPE_REGISTER} does not exist")
endif()

file(READ "${RTTR_TYPE_REGISTER}" CONTENTS)

set(BROKEN_SORT "    std::sort(base_classes.begin(), base_classes.end(), [](const base_class_info& left, const base_class_info& right)\n                                                         { return left.m_base_type.is_base_of(right.m_base_type); });")

set(FIXED_SORT "    std::stable_sort(base_classes.begin(), base_classes.end(), [](const base_class_info& left, const base_class_info& right)\n                                                         { return left.m_base_type.get_base_classes().size() < right.m_base_type.get_base_classes().size(); });")

string(FIND "${CONTENTS}" "${FIXED_SORT}" ALREADY_PATCHED)

if(NOT ALREADY_PATCHED EQUAL -1)
    message(STATUS "RTTR: base-class sort already patched")
    return()
endif()

string(FIND "${CONTENTS}" "${BROKEN_SORT}" FOUND_BROKEN)

if(FOUND_BROKEN EQUAL -1)
    # Loud rather than silent: a version bump that rewrote this call would
    # otherwise quietly reintroduce the abort on device.
    message(FATAL_ERROR
        "RTTR: the base-class sort in type_register.cpp does not match what "
        "CMake/PatchRTTR.cmake expects. Re-check the upstream source against "
        "the comment in that file before bumping VOXAGINE_RTTR_TAG.")
endif()

string(REPLACE "${BROKEN_SORT}" "${FIXED_SORT}" CONTENTS "${CONTENTS}")
file(WRITE "${RTTR_TYPE_REGISTER}" "${CONTENTS}")

message(STATUS "RTTR: patched the base-class sort to a valid strict weak ordering")
