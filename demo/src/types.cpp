#include "types.hpp"

// Framerated's constructor, ScaleUp() and ScaleDown() are constexpr, so their
// definitions live in types.hpp -- a constexpr template has to be visible
// at every call site
// across translation units, not just this one. What belongs here is
// explicit instantiation of the combinations the demo actually uses, which
// both forces their static_asserts to run at build time and catches
// use-before-declaration mistakes early.
namespace demo {

template class Framerated<u8, 60>;
template class Framerated<u16, 256>;
template class Framerated<i16, -100>;

}
