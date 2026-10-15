#include "st_stacktrace.hpp"
#include <cstdio>
#include <cstdlib>
#include <new>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static unsigned allocations=0;
void* operator new(std::size_t n) { ++allocations; if(void* p=std::malloc(n?n:1)) return p; std::abort(); }
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
__attribute__((noinline)) void measured_capture() {
    void* addresses[64];
    allocations=0;
    auto n=st::capture(addresses,64);
    auto used=allocations;
    std::printf("st::capture frames=%zu operator_new_calls=%u\n",n,used);
    allocations=0;
    auto win_n=CaptureStackBackTrace(0,64,addresses,nullptr);
    std::printf("CaptureStackBackTrace frames=%u operator_new_calls=%u\n",unsigned(win_n),allocations);
    st::Options options; options.depth=2; options.addresses=true;
    auto text=st::trace_to_string(options);
    std::printf("requested_depth=2\n%s",text.c_str());
}
int main() {measured_capture();return 0;}
