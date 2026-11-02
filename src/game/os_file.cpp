// os_file: the game's buffered Win32 file wrapper (FUN_0058e020 and friends).
#include <windows.h>
#include <cstddef>
#include <cstdint>

#include "../remade.h"

namespace {

// Raw layout of the original object; only the fields the remade functions touch.
struct os_file {
    uint8_t unk0[0x10];
    uint32_t flags;      // +0x10: bit 0 read-buffered, bits 1-2 write-buffered
    uint8_t unk14[4];
    HANDLE handle;       // +0x18
    int32_t pos;         // +0x1c: logical file position
    int32_t size;        // +0x20
    uint8_t buffered;    // +0x24
    uint8_t unk25[3];
    int32_t buf_index;   // +0x28
    int32_t buf_pos;     // +0x2c: file position of the buffer
    int32_t buf_fill;    // +0x30
};
static_assert(offsetof(os_file, flags) == 0x10);
static_assert(offsetof(os_file, handle) == 0x18);
static_assert(offsetof(os_file, pos) == 0x1c);
static_assert(offsetof(os_file, size) == 0x20);
static_assert(offsetof(os_file, buffered) == 0x24);
static_assert(offsetof(os_file, buf_pos) == 0x2c);
static_assert(offsetof(os_file, buf_fill) == 0x30);

// Not remade yet: called in the original.
auto const flush_write_buffer = (void(__thiscall*)(os_file*))0x58df50;
auto const refill_read_buffer = (uint32_t(__thiscall*)(os_file*))0x58def0;

}  // namespace

// FUN_0058e020: seek. whence 0/1/2 = begin/current/end; anything else seeks from the start without
// moving `pos`, as the original does.
// Note: SetFilePointer gets the raw `offset`, not the new `pos`, matching the original.
uint32_t __thiscall os_file_seek(void* self, int32_t offset, int32_t whence) {
    REMADE_HIT(os_file_seek);
    auto f = (os_file*)self;
    if (f->buffered && (f->flags & 6) && f->buf_pos) flush_write_buffer(f);

    DWORD method = FILE_BEGIN;
    if (whence == 0) {
        f->pos = offset;
    } else if (whence == 1) {
        f->pos += offset;
        method = FILE_CURRENT;
    } else if (whence == 2) {
        f->pos = f->size + offset;
        method = FILE_END;
    }
    uint32_t r = SetFilePointer(f->handle, offset, nullptr, method);

    // The original reuses eax: its low byte ends up holding `buffered` unless the refill runs.
    r = (r & 0xffffff00u) | f->buffered;
    if (f->buffered && (f->flags & 1)) {
        f->buf_pos = f->pos;
        f->buf_fill = 0;
        r = refill_read_buffer(f);
    }
    return r;
}
