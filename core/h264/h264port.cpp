// MP4 Player 9300: allocation for the h264bsd decoder on the phone. It uses the current thread
// heap; the playback engine switches to its own decoder heap (User::SwitchHeap) around every call
// into the video decoder, so the H.264 picture buffers come from that chunk heap.
#include <e32std.h>

extern "C" void* h264port_malloc(unsigned int aSize) { return User::Alloc((TInt)aSize); }
extern "C" void h264port_free(void* aPtr) { User::Free(aPtr); }
