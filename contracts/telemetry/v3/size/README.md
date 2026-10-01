# Serialized-size review

The committed fixtures are measured as exact UTF-8 bytes. The minimal request is
703 bytes and the pretty-print equivalent is 891 bytes. The canonical record fixture
is 466 bytes before its hash field. The valid corpus ranges from 664 bytes for the
Unicode metadata case to 1,606 bytes for the mixed batch; the near-limit result is
1,042 bytes after the typed trace freeze. The frozen ceilings remain 16 KiB per
record and 32 KiB per request. The invalid record-too-large fixture remains an
independent regression for the 16 KiB boundary.
Full source sample IDs, raw evidence, and model traces are never truncated; a future
fixture that exceeds a ceiling is a contract/storage defect.
