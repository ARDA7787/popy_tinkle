// Attestation: golden canonical-payload bytes, per-field tamper detection,
// the filename-transplant defence, and the missing/unknown-alg states.

#include <string>

#include "core/attest.h"
#include "core/sidecar.h"
#include "test_main.h"

using namespace popy::test;
using popy::attest::VerifyResult;

namespace {

popy::sidecar::Record fixture() {
  popy::sidecar::Record r;
  r.id               = "00000000-0000-4000-8000-000000000001";
  r.diskPath         = "/stage/x/report.pdf_popy";
  r.originalFilename = "report.pdf";
  r.sourceUrl        = "https://example.com/report.pdf";
  r.sourceHost       = "example.com";
  r.sizeBytes        = 12345;
  r.mime             = "application/pdf";
  r.sha256           = "aa11bb22";
  r.status           = "stored";
  r.createdAt        = 1700000000000;
  r.origin           = "fetch";
  return r;
}

const std::string kKey(32, 'k');

}  // namespace

int main() {
  POPY_RUN("golden canonical payload bytes") {
    auto p = popy::attest::canonical_payload(fixture(), "report.pdf_popy");
    std::string expected =
        "popy-attest-v1\n"
        "schema:1:1\n"
        "id:36:00000000-0000-4000-8000-000000000001\n"
        "name:15:report.pdf_popy\n"
        "orig:10:report.pdf\n"
        "sha256:8:aa11bb22\n"
        "size:5:12345\n"
        "mime:15:application/pdf\n"
        "url:30:https://example.com/report.pdf\n"
        "host:11:example.com\n"
        "created:13:1700000000000\n"
        "status:6:stored\n"
        "origin:5:fetch\n";
    POPY_EXPECT_EQ(p, expected);
  };

  POPY_RUN("sign then verify ok") {
    auto r = fixture();
    popy::attest::sign(r, "report.pdf_popy", kKey);
    POPY_EXPECT(r.sig.has_value());
    POPY_EXPECT_EQ(r.sig->size(), static_cast<size_t>(64));
    POPY_EXPECT_EQ(r.sigAlg.value_or(""), std::string(popy::attest::kSigAlg));
    POPY_EXPECT(popy::attest::verify(r, "report.pdf_popy", kKey) ==
                VerifyResult::ok);
  };

  POPY_RUN("per-field tamper flips to bad_signature") {
    auto base = fixture();
    popy::attest::sign(base, "report.pdf_popy", kKey);

    auto expect_bad = [&](popy::sidecar::Record r) {
      POPY_EXPECT(popy::attest::verify(r, "report.pdf_popy", kKey) ==
                  VerifyResult::bad_signature);
    };
    { auto r = base; r.sha256 = "aa11bb23";                 expect_bad(r); }
    { auto r = base; r.sizeBytes = 12346;                   expect_bad(r); }
    { auto r = base; r.originalFilename = "report2.pdf";    expect_bad(r); }
    { auto r = base; r.mime = "text/plain";                 expect_bad(r); }
    { auto r = base; r.sourceUrl = "https://evil.example/"; expect_bad(r); }
    { auto r = base; r.sourceHost = "evil.example";         expect_bad(r); }
    { auto r = base; r.status = "released";                 expect_bad(r); }
    { auto r = base; r.origin = "watcher";                  expect_bad(r); }
    { auto r = base; r.id = "00000000-0000-4000-8000-000000000002"; expect_bad(r); }
    { auto r = base; r.createdAt = 1700000000001;           expect_bad(r); }
  };

  POPY_RUN("length-prefix framing: shifting bytes between fields fails") {
    // "orig" ends with what "sha256" begins with — framing must not allow
    // ambiguity. Sign one record, tamper two fields so their concatenation
    // is unchanged.
    auto r = fixture();
    r.originalFilename = "ab";
    r.sha256 = "cd";
    popy::attest::sign(r, "n", kKey);
    auto t = r;
    t.originalFilename = "abc";
    t.sha256 = "d";
    POPY_EXPECT(popy::attest::verify(t, "n", kKey) ==
                VerifyResult::bad_signature);
  };

  POPY_RUN("sidecar transplant: signature is pinned to the on-disk name") {
    auto r = fixture();
    popy::attest::sign(r, "report.pdf_popy", kKey);
    POPY_EXPECT(popy::attest::verify(r, "other.pdf_popy", kKey) ==
                VerifyResult::bad_signature);
    POPY_EXPECT(popy::attest::verify(r, "report.pdf_popy.1", kKey) ==
                VerifyResult::bad_signature);
  };

  POPY_RUN("wrong key fails") {
    auto r = fixture();
    popy::attest::sign(r, "report.pdf_popy", kKey);
    POPY_EXPECT(popy::attest::verify(r, "report.pdf_popy",
                                     std::string(32, 'x')) ==
                VerifyResult::bad_signature);
  };

  POPY_RUN("missing signature detected") {
    auto r = fixture();
    POPY_EXPECT(popy::attest::verify(r, "report.pdf_popy", kKey) ==
                VerifyResult::missing_signature);
    POPY_EXPECT_THROW(popy::attest::verify_or_throw(r, "report.pdf_popy", kKey));
  };

  POPY_RUN("unknown algorithm detected") {
    auto r = fixture();
    popy::attest::sign(r, "report.pdf_popy", kKey);
    r.sigAlg = "md5-lol";
    POPY_EXPECT(popy::attest::verify(r, "report.pdf_popy", kKey) ==
                VerifyResult::unknown_alg);
  };

  return exit_code();
}
