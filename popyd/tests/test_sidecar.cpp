#include <cstdio>
#include <filesystem>
#include <fstream>

#include "core/sidecar.h"
#include "test_main.h"

namespace fs = std::filesystem;
using namespace popy::test;

int main() {
  fs::path tmp = fs::temp_directory_path() / "popy_test_sidecar";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  POPY_RUN("round-trip") {
    popy::sidecar::Record r;
    r.id               = "00000000-0000-4000-8000-000000000001";
    r.diskPath         = "stage/foo_popy";
    r.originalFilename = "foo.pdf";
    r.sourceUrl        = "https://example.com/foo.pdf";
    r.sourceHost       = "example.com";
    r.sizeBytes        = 12345;
    r.mime             = "application/pdf";
    r.sha256           = "abc";
    r.path             = "fallback";
    r.status           = "stored";
    r.createdAt        = 1700000000000;
    r.note             = std::string("hello");

    auto p = tmp / "foo_popy.meta.json";
    popy::sidecar::write_atomic(p, r);

    auto back = popy::sidecar::read(p);
    POPY_EXPECT_EQ(back.id,               r.id);
    POPY_EXPECT_EQ(back.originalFilename, r.originalFilename);
    POPY_EXPECT_EQ(back.sourceUrl,        r.sourceUrl);
    POPY_EXPECT_EQ(back.sizeBytes,        r.sizeBytes);
    POPY_EXPECT_EQ(back.sha256,           r.sha256);
    POPY_EXPECT_EQ(back.status,           r.status);
    POPY_EXPECT_EQ(back.note.value_or(""), std::string("hello"));
    POPY_EXPECT_EQ(back.schemaVersion,    popy::sidecar::kSchemaVersion);
  };

  POPY_RUN("sig/sigAlg round-trip") {
    popy::sidecar::Record r;
    r.id               = "00000000-0000-4000-8000-000000000002";
    r.originalFilename = "signed.bin";
    r.createdAt        = 1700000000000;
    r.sig              = std::string(64, 'a');
    r.sigAlg           = std::string("hmac-sha256-v1");

    auto p = tmp / "signed_popy.meta.json";
    popy::sidecar::write_atomic(p, r);
    auto back = popy::sidecar::read(p);
    POPY_EXPECT_EQ(back.sig.value_or(""),    std::string(64, 'a'));
    POPY_EXPECT_EQ(back.sigAlg.value_or(""), std::string("hmac-sha256-v1"));
  };

  POPY_RUN("pre-upgrade sidecar reads with no signature") {
    popy::sidecar::Record r;
    r.id               = "00000000-0000-4000-8000-000000000003";
    r.originalFilename = "old.bin";
    r.createdAt        = 1700000000000;

    auto p = tmp / "old_popy.meta.json";
    popy::sidecar::write_atomic(p, r);
    auto back = popy::sidecar::read(p);
    POPY_EXPECT(!back.sig.has_value());
    POPY_EXPECT(!back.sigAlg.has_value());
  };

  POPY_RUN("required keys") {
    auto p = tmp / "bad.meta.json";
    {
      std::ofstream f(p);
      f << "{\"id\":\"x\"}";
    }
    POPY_EXPECT_THROW(popy::sidecar::read(p));
  };

  POPY_RUN("invalid json") {
    auto p = tmp / "junk.meta.json";
    {
      std::ofstream f(p);
      f << "not json {{";
    }
    POPY_EXPECT_THROW(popy::sidecar::read(p));
  };

  POPY_RUN("missing file") {
    POPY_EXPECT_THROW(popy::sidecar::read(tmp / "does-not-exist.meta.json"));
  };

  fs::remove_all(tmp);
  return exit_code();
}
