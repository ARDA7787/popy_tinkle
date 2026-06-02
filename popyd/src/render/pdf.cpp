#include "render/pdf.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

#include <mupdf/fitz.h>

namespace popy::render {

namespace {

constexpr std::size_t kMaxText = 64U * 1024U;
constexpr std::size_t kMaxPng = 2U * 1024U * 1024U;

struct Context {
  fz_context* ctx = nullptr;
  Context() {
    ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (ctx == nullptr) throw std::runtime_error("mupdf: context allocation failed");
    fz_register_document_handlers(ctx);
  }
  ~Context() {
    if (ctx != nullptr) fz_drop_context(ctx);
  }
};

struct FdFile {
  FILE* file = nullptr;
  explicit FdFile(int fd) {
    int dup_fd = ::dup(fd);
    if (dup_fd < 0) {
      throw std::runtime_error(std::string("dup render fd failed: ") +
                               std::strerror(errno));
    }
    file = ::fdopen(dup_fd, "rb");
    if (file == nullptr) {
      int e = errno;
      ::close(dup_fd);
      throw std::runtime_error(std::string("fdopen render fd failed: ") +
                               std::strerror(e));
    }
  }
  FdFile(const FdFile&) = delete;
  FdFile& operator=(const FdFile&) = delete;
  ~FdFile() {
    if (file != nullptr) {
      ::fclose(file);
    }
  }
};

std::string truncate_text(std::string text) {
  if (text.size() <= kMaxText) return text;
  text.resize(kMaxText);
  text += "\n[truncated]\n";
  return text;
}

std::vector<std::uint8_t> buffer_bytes(fz_context* ctx, fz_buffer* buffer) {
  unsigned char* data = nullptr;
  std::size_t len = fz_buffer_storage(ctx, buffer, &data);
  if (len > kMaxPng) {
    throw std::runtime_error("rendered PNG exceeded 2MB output cap");
  }
  return std::vector<std::uint8_t>(data, data + len);
}

}  // namespace

PdfInfo extract_text(int fd) {
  Context c;
  FdFile file(fd);
  PdfInfo out;
  fz_stream* stream = nullptr;
  fz_document* doc = nullptr;
  fz_page* page = nullptr;
  fz_buffer* text = nullptr;

  fz_try(c.ctx) {
    stream = fz_open_file_ptr_no_close(c.ctx, file.file);
    doc = fz_open_document_with_stream(c.ctx, "pdf", stream);
    out.page_count = fz_count_pages(c.ctx, doc);
    if (out.page_count > 0) {
      page = fz_load_page(c.ctx, doc, 0);
      text = fz_new_buffer_from_page(c.ctx, page, nullptr);
      unsigned char* data = nullptr;
      std::size_t len = fz_buffer_storage(c.ctx, text, &data);
      out.text_sample.assign(reinterpret_cast<const char*>(data), len);
      out.text_sample = truncate_text(std::move(out.text_sample));
    }
  }
  fz_always(c.ctx) {
    fz_drop_buffer(c.ctx, text);
    fz_drop_page(c.ctx, page);
    fz_drop_document(c.ctx, doc);
    fz_drop_stream(c.ctx, stream);
  }
  fz_catch(c.ctx) {
    throw std::runtime_error(std::string("mupdf text extraction failed: ") +
                             fz_caught_message(c.ctx));
  }

  return out;
}

std::vector<std::uint8_t> render_page_png(int fd, int page_num) {
  Context c;
  FdFile file(fd);
  std::vector<std::uint8_t> out;
  fz_stream* stream = nullptr;
  fz_document* doc = nullptr;
  fz_page* page = nullptr;
  fz_pixmap* pix = nullptr;
  fz_buffer* png = nullptr;

  fz_try(c.ctx) {
    stream = fz_open_file_ptr_no_close(c.ctx, file.file);
    doc = fz_open_document_with_stream(c.ctx, "pdf", stream);
    int pages = fz_count_pages(c.ctx, doc);
    if (page_num < 0 || page_num >= pages) {
      fz_throw(c.ctx, FZ_ERROR_GENERIC, "page out of range");
    }
    page = fz_load_page(c.ctx, doc, page_num);
    fz_matrix ctm = fz_scale(1.5F, 1.5F);
    pix = fz_new_pixmap_from_page(c.ctx, page, ctm, fz_device_rgb(c.ctx), 0);
    png = fz_new_buffer_from_pixmap_as_png(c.ctx, pix, fz_default_color_params);
    out = buffer_bytes(c.ctx, png);
  }
  fz_always(c.ctx) {
    fz_drop_buffer(c.ctx, png);
    fz_drop_pixmap(c.ctx, pix);
    fz_drop_page(c.ctx, page);
    fz_drop_document(c.ctx, doc);
    fz_drop_stream(c.ctx, stream);
  }
  fz_catch(c.ctx) {
    throw std::runtime_error(std::string("mupdf render failed: ") +
                             fz_caught_message(c.ctx));
  }

  return out;
}

}  // namespace popy::render
