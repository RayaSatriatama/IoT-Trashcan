# Serialized-size review

The committed fixtures are measured as exact UTF-8 bytes. The minimal request is
703 bytes and the pretty-print equivalent is 891 bytes. The canonical record fixture
is 465 bytes before its hash field. The valid corpus ranges from 632 bytes for the
clock-anchor event to 13,019 bytes for the near-limit result; the mixed batch is
1,513 bytes. The frozen ceilings remain 16 KiB per record and 32 KiB per request.
Full source sample IDs, raw evidence, and model traces are never truncated; a future
fixture that exceeds a ceiling is a contract/storage defect.
