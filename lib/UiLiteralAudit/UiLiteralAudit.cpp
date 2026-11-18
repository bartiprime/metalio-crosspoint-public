#include "UiLiteralAudit.h"

#include "UiLiteralAudit.generated.h"

namespace uiLiteralAudit {

int count() { return uiLiteralAuditGen::kCount; }

int fileCount() { return uiLiteralAuditGen::kFileCount; }

const char* entry(int index) {
  if (index < 0 || index >= uiLiteralAuditGen::kCount) return "";
  return uiLiteralAuditGen::kEntries[index];
}

}  // namespace uiLiteralAudit
