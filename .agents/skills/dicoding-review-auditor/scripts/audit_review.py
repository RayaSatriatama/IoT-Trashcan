#!/usr/bin/env python3
"""Validate a Dicoding review profile and packet without third-party packages."""
from __future__ import annotations
import argparse
import copy
import hashlib
import json
import math
import re
import sys
from datetime import datetime
from pathlib import Path
from typing import Any, Iterable
from urllib.parse import urlparse
VERSION = "1.1.0"
ROOT = Path(__file__).resolve().parents[1]
PROFILE_SCHEMA_PATH = ROOT / "references" / "class-profile.schema.json"
PACKET_SCHEMA_PATH = ROOT / "references" / "review-packet.schema.json"
FINAL_DECISIONS = {"LULUS", "REVISI"}
PASSING_MANDATORY = {"MET", "SKIPPED", "NOT_APPLICABLE"}
NEGATIVE_STATUSES = {"NOT_MET", "PARTIAL", "BLOCKED"}
DECISIVE_RULE_ROLES = {"official_gate", "common_case"}
NON_DECISIVE_RULE_ROLES = {"diagnostic_case", "development_note"}
AUTHORITATIVE_CLASSES = {"AUTHORITATIVE_REQUIREMENT", "AUTHORITATIVE_EXCEPTION"}
OFFICIAL_AUTHORITY_KINDS = {"official_rubric", "official_exception", "official_common_case"}
AUTHORITY_CHAIN_RANK = {
    "official_rubric": 0,
    "official_exception": 1,
    "official_common_case": 1,
    "class_skill": 2,
    "feedback_standard": 3,
    "automation": 4,
}
RUNTIME_ROLE_CAPABILITIES = {
    "notebook_execution": {"runtime"},
    "source_execution": {"runtime"},
    "test_suite": {"runtime"},
    "browser_interaction": {"browser"},
    "http_probe": {"runtime"},
    "repository_access": {"source_analysis"},
    "external_service": {"runtime"},
}
ROLE_CAPABILITY_ITEM_KINDS = {
    "filesystem": "FILESYSTEM",
    "runtime": "PYTHON_RUNTIME",
    "browser": "BROWSER",
}
PLACEHOLDER_PATTERN = re.compile(r"(?:\{\{[^}]+\}\}|\bTODO\b|\bTBD\b|\bPLACEHOLDER\b|\bUNKNOWN\b|\bNOT[ _-]?RECORDED\b)", re.IGNORECASE)
def _issue(code: str, path: str, message: str, severity: str = "ERROR") -> dict[str, str]:
    return {"code": code, "path": path, "message": message, "severity": severity}
def _canonical_sha256(value: Any) -> str:
    payload = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()
def _packet_subject_sha256(packet: dict[str, Any]) -> str:
    subject = copy.deepcopy(packet)
    subject.get("audit", {})["validator_runs"] = []
    return _canonical_sha256(subject)
def _local_path(reference: Any) -> Path | None:
    if not isinstance(reference, str):
        return None
    value = reference[7:] if reference.startswith("file://") else reference
    path = Path(value)
    return path if path.is_absolute() else None
def _verify_local_digest(reference: Any, digest: Any, path: str, issues: list[dict[str, str]]) -> None:
    local = _local_path(reference)
    if local is None or not local.is_file() or not isinstance(digest, str):
        return
    try:
        actual = hashlib.sha256(local.read_bytes()).hexdigest()
    except OSError:
        return
    if actual.lower() != digest.lower():
        issues.append(_issue("REFERENCE_DIGEST_MISMATCH", path, "recorded digest differs from the referenced local file"))
def _authority_ids(refs: Any) -> list[Any]:
    if not isinstance(refs, list):
        return []
    return [item.get("authority_id") for item in refs if isinstance(item, dict)]
def _contains_absolute_posix_path(value: str) -> bool:
    return re.search(r"(?:^|[\s(\[{'\"])(/(?!/)[^\s<>\"'`)]*)", value) is not None
def _json_type_matches(value: Any, expected: str) -> bool:
    if expected == "object":
        return isinstance(value, dict)
    if expected == "array":
        return isinstance(value, list)
    if expected == "string":
        return isinstance(value, str)
    if expected == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if expected == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if expected == "boolean":
        return isinstance(value, bool)
    if expected == "null":
        return value is None
    return True
class SchemaValidator:
    """Small Draft 2020-12 subset covering the bundled schemas."""
    def __init__(self, root_schema: dict[str, Any], label: str):
        self.root = root_schema
        self.label = label
    def validate(self, value: Any) -> list[dict[str, str]]:
        messages: list[tuple[str, str]] = []
        self._walk(value, self.root, "$", messages)
        return [
            _issue("SCHEMA_VALIDATION", f"{self.label}{path[1:]}", message)
            for path, message in messages
        ]
    def _resolve(self, reference: str) -> dict[str, Any]:
        if not reference.startswith("#/"):
            raise ValueError(f"Unsupported schema reference: {reference}")
        current: Any = self.root
        for part in reference[2:].split("/"):
            current = current[part.replace("~1", "/").replace("~0", "~")]
        return current
    def _matches(self, value: Any, schema: dict[str, Any]) -> bool:
        probe: list[tuple[str, str]] = []
        self._walk(value, schema, "$", probe)
        return not probe
    def _walk(
        self,
        value: Any,
        schema: dict[str, Any],
        path: str,
        messages: list[tuple[str, str]],
    ) -> None:
        if "$ref" in schema:
            self._walk(value, self._resolve(schema["$ref"]), path, messages)
            return
        expected = schema.get("type")
        if expected is not None:
            expected_types = expected if isinstance(expected, list) else [expected]
            if not any(_json_type_matches(value, item) for item in expected_types):
                messages.append((path, f"expected type {expected!r}"))
                return
        if "const" in schema and value != schema["const"]:
            messages.append((path, f"must equal {schema['const']!r}"))
        if "enum" in schema and value not in schema["enum"]:
            messages.append((path, f"must be one of {schema['enum']!r}"))
        if isinstance(value, dict):
            required = schema.get("required", [])
            for name in required:
                if name not in value:
                    messages.append((f"{path}.{name}", "required property is missing"))
            properties = schema.get("properties", {})
            if schema.get("additionalProperties") is False:
                for name in value:
                    if name not in properties:
                        messages.append((f"{path}.{name}", "additional property is not allowed"))
            for name, child_schema in properties.items():
                if name in value:
                    self._walk(value[name], child_schema, f"{path}.{name}", messages)
        if isinstance(value, list):
            if len(value) < schema.get("minItems", 0):
                messages.append((path, f"must contain at least {schema['minItems']} items"))
            if "maxItems" in schema and len(value) > schema["maxItems"]:
                messages.append((path, f"must contain at most {schema['maxItems']} items"))
            if schema.get("uniqueItems"):
                seen: set[str] = set()
                for index, item in enumerate(value):
                    marker = json.dumps(item, sort_keys=True, separators=(",", ":"))
                    if marker in seen:
                        messages.append((f"{path}[{index}]", "array item must be unique"))
                    seen.add(marker)
            item_schema = schema.get("items")
            if item_schema:
                for index, item in enumerate(value):
                    self._walk(item, item_schema, f"{path}[{index}]", messages)
        if isinstance(value, str):
            if len(value) < schema.get("minLength", 0):
                messages.append((path, f"must contain at least {schema['minLength']} characters"))
            if "maxLength" in schema and len(value) > schema["maxLength"]:
                messages.append((path, f"must contain at most {schema['maxLength']} characters"))
            if "pattern" in schema and re.search(schema["pattern"], value) is None:
                messages.append((path, f"does not match pattern {schema['pattern']!r}"))
            if schema.get("format") == "date-time" and not _valid_datetime(value):
                messages.append((path, "must be an RFC 3339 date-time"))
            if schema.get("format") == "uri" and not _valid_uri(value):
                messages.append((path, "must be an absolute URI"))
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            if "minimum" in schema and value < schema["minimum"]:
                messages.append((path, f"must be at least {schema['minimum']}"))
            if "maximum" in schema and value > schema["maximum"]:
                messages.append((path, f"must be at most {schema['maximum']}"))
        for child in schema.get("allOf", []):
            if "if" in child:
                if self._matches(value, child["if"]):
                    self._walk(value, child.get("then", {}), path, messages)
                elif "else" in child:
                    self._walk(value, child["else"], path, messages)
            else:
                self._walk(value, child, path, messages)
def _valid_datetime(value: str) -> bool:
    try:
        datetime.fromisoformat(value.replace("Z", "+00:00"))
        return "T" in value
    except ValueError:
        return False
def _valid_uri(value: str) -> bool:
    parsed = urlparse(value)
    return bool(parsed.scheme and (parsed.netloc or parsed.path))
def _by_id(items: Iterable[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    return {item.get("id"): item for item in items if isinstance(item, dict) and item.get("id")}
def _check_unique(items: Any, path: str, issues: list[dict[str, str]]) -> None:
    if not isinstance(items, list):
        return
    for field, code in (("id", "DUPLICATE_ID"), ("order", "DUPLICATE_ORDER")):
        seen: dict[Any, int] = {}
        for index, item in enumerate(items):
            if not isinstance(item, dict) or field not in item:
                continue
            value = item[field]
            if value in seen:
                issues.append(_issue(code, f"{path}[{index}].{field}", f"{field} duplicates item {seen[value]}"))
            else:
                seen[value] = index
def _arrays(document: dict[str, Any], prefix: str) -> Iterable[tuple[str, Any]]:
    for name in (
        "authority_sources", "artifact_types", "criteria", "runtime_obligations",
        "delegation_roles", "feedback_templates", "skip_rules", "automation_runs",
        "evidence", "submission_artifacts", "runtime_results", "crosschecks", "artifacts",
    ):
        if name in document:
            yield f"{prefix}.{name}", document[name]
    capabilities = document.get("capabilities", {})
    if isinstance(capabilities, dict):
        yield f"{prefix}.capabilities.items", capabilities.get("items", [])
        yield f"{prefix}.capabilities.role_assignments", capabilities.get("role_assignments", [])
    context = document.get("student_context", {})
    if isinstance(context, dict):
        yield f"{prefix}.student_context.notes", context.get("notes", [])
        yield f"{prefix}.student_context.history", context.get("history", [])
    integrity = document.get("integrity", {})
    if isinstance(integrity, dict):
        yield f"{prefix}.integrity.checks", integrity.get("checks", [])
    audit_data = document.get("audit", {})
    if isinstance(audit_data, dict):
        yield f"{prefix}.audit.validator_runs", audit_data.get("validator_runs", [])
def _refs(
    refs: Any,
    known: set[str],
    code: str,
    path: str,
    issues: list[dict[str, str]],
) -> None:
    if not isinstance(refs, list):
        return
    for index, reference in enumerate(refs):
        if reference not in known:
            issues.append(_issue(code, f"{path}[{index}]", f"unknown reference {reference!r}"))
def _validate_rule_metadata(reference: Any, path: str, issues: list[dict[str, str]]) -> None:
    if not isinstance(reference, dict):
        return
    role = reference.get("rule_role")
    authority_class = reference.get("authority_class")
    outcome_effect = reference.get("outcome_effect")
    if role in DECISIVE_RULE_ROLES:
        if outcome_effect is not True:
            issues.append(_issue(
                "RULE_METADATA_INCONSISTENT",
                f"{path}.outcome_effect",
                f"{role} must be outcome-affecting",
            ))
        if authority_class not in AUTHORITATIVE_CLASSES:
            issues.append(_issue(
                "RULE_METADATA_INCONSISTENT",
                f"{path}.authority_class",
                f"{role} needs an authoritative authority_class",
            ))
    elif role in NON_DECISIVE_RULE_ROLES:
        if outcome_effect is not False:
            issues.append(_issue(
                "RULE_METADATA_INCONSISTENT",
                f"{path}.outcome_effect",
                f"{role} cannot affect an outcome independently",
            ))
        if authority_class in AUTHORITATIVE_CLASSES:
            issues.append(_issue(
                "RULE_METADATA_INCONSISTENT",
                f"{path}.authority_class",
                f"{role} cannot claim an outcome-authoritative authority_class",
            ))
def _profile_semantics(profile: dict[str, Any], issues: list[dict[str, str]]) -> None:
    for path, items in _arrays(profile, "profile"):
        _check_unique(items, path, issues)
    authority_sources = _by_id(profile.get("authority_sources", []))
    authority_ids = set(authority_sources)
    authority_kinds = {item.get("kind") for item in profile.get("authority_sources", []) if isinstance(item, dict)}
    for required_kind in ("official_rubric", "class_skill"):
        if required_kind not in authority_kinds:
            issues.append(_issue("OFFICIAL_AUTHORITY_REQUIRED", "profile.authority_sources", f"authority kind {required_kind!r} is required"))
    present_ranks = {AUTHORITY_CHAIN_RANK.get(kind) for kind in authority_kinds}
    missing_ranks = sorted(set(range(5)) - present_ranks)
    if missing_ranks:
        issues.append(_issue("AUTHORITY_CHAIN_INCOMPLETE", "profile.authority_sources", f"authority chain is missing ranks {missing_ranks!r}"))
    ordered_sources = sorted(
        (item for item in profile.get("authority_sources", []) if isinstance(item, dict)),
        key=lambda item: item.get("order", math.inf),
    )
    ordered_ranks = [AUTHORITY_CHAIN_RANK.get(item.get("kind"), math.inf) for item in ordered_sources]
    if ordered_ranks != sorted(ordered_ranks):
        issues.append(_issue("AUTHORITY_CHAIN_ORDER", "profile.authority_sources", "authority sources must follow rubric, exceptions/common cases, class skill, feedback standard, automation"))
    for index, source in enumerate(profile.get("authority_sources", [])):
        if not isinstance(source, dict):
            continue
        if source.get("sha256"):
            _verify_local_digest(source.get("reference"), source.get("sha256"), f"profile.authority_sources[{index}].sha256", issues)
        not_applicable_ref = source.get("not_applicable_authority_ref")
        if source.get("applicability") == "NOT_APPLICABLE":
            cited = authority_sources.get(not_applicable_ref.get("authority_id")) if isinstance(not_applicable_ref, dict) else None
            if (
                cited is None
                or cited.get("kind") not in OFFICIAL_AUTHORITY_KINDS
                or cited.get("applicability") != "ACTIVE"
                or cited.get("order", math.inf) >= source.get("order", -1)
                or not str(not_applicable_ref.get("locator", "")).strip()
            ):
                issues.append(_issue("AUTHORITY_NOT_APPLICABLE_UNAUTHORIZED", f"profile.authority_sources[{index}].not_applicable_authority_ref", "not-applicable authority category must cite a prior active official authority with a precise locator"))
        elif not_applicable_ref is not None:
            issues.append(_issue("AUTHORITY_NOT_APPLICABLE_MISMATCH", f"profile.authority_sources[{index}].not_applicable_authority_ref", "active authority source cannot carry a not-applicable declaration"))
    for index, rule in enumerate(profile.get("skip_rules", [])):
        if not isinstance(rule, dict):
            continue
        reference = rule.get("authority_ref")
        if not isinstance(reference, dict) or not str(reference.get("locator", "")).strip():
            issues.append(_issue("SKIP_RULE_AUTHORITY_REQUIRED", f"profile.skip_rules[{index}].authority_ref", "skip rule needs an authoritative source ID and precise locator"))
            continue
        source = authority_sources.get(reference.get("authority_id"))
        if source is None:
            issues.append(_issue("BROKEN_PROFILE_REF", f"profile.skip_rules[{index}].authority_ref", "unknown authority source"))
        elif source.get("kind") not in OFFICIAL_AUTHORITY_KINDS or source.get("applicability") != "ACTIVE":
            issues.append(_issue("SKIP_RULE_AUTHORITY_INVALID", f"profile.skip_rules[{index}].authority_ref", "skip rule authority must be an active official rubric, exception, or common-case source"))
    artifact_type_ids = set(_by_id(profile.get("artifact_types", [])))
    obligation_ids = set(_by_id(profile.get("runtime_obligations", [])))
    for index, criterion in enumerate(profile.get("criteria", [])):
        _refs(_authority_ids(criterion.get("authority_refs")), authority_ids, "BROKEN_PROFILE_REF", f"profile.criteria[{index}].authority_refs", issues)
        _refs(criterion.get("runtime_obligation_refs"), obligation_ids, "BROKEN_PROFILE_REF", f"profile.criteria[{index}].runtime_obligation_refs", issues)
        if profile.get("schema_version") == "1.1.0":
            labels = criterion.get("audience_labels", {})
            reviewer = labels.get("reviewer", {}) if isinstance(labels, dict) else {}
            if criterion.get("official_id") != reviewer.get("id"):
                issues.append(_issue(
                    "CRITERION_AUDIENCE_LABEL_MISMATCH",
                    f"profile.criteria[{index}].official_id",
                    "official_id must equal audience_labels.reviewer.id",
                ))
            for reference_index, reference in enumerate(criterion.get("authority_refs", [])):
                _validate_rule_metadata(
                    reference,
                    f"profile.criteria[{index}].authority_refs[{reference_index}]",
                    issues,
                )
    for index, obligation in enumerate(profile.get("runtime_obligations", [])):
        trigger = obligation.get("trigger", {}) if isinstance(obligation, dict) else {}
        if trigger.get("type") == "ARTIFACT_PRESENT" and trigger.get("artifact_type_ref") not in artifact_type_ids:
            issues.append(_issue("BROKEN_PROFILE_REF", f"profile.runtime_obligations[{index}].trigger.artifact_type_ref", "unknown artifact type"))
    tier_ranges: list[tuple[float, float, int]] = []
    for index, tier in enumerate(profile.get("scoring", {}).get("tiers", [])):
        minimum, maximum = tier.get("minimum"), tier.get("maximum")
        if isinstance(minimum, (int, float)) and isinstance(maximum, (int, float)):
            if minimum > maximum:
                issues.append(_issue("INVALID_SCORING_POLICY", f"profile.scoring.tiers[{index}]", "minimum exceeds maximum"))
            tier_ranges.append((minimum, maximum, index))
        if profile.get("scoring", {}).get("mode") == "optional_count" and not isinstance(tier.get("value"), (int, float)):
            issues.append(_issue("INVALID_SCORING_POLICY", f"profile.scoring.tiers[{index}].value", "optional_count tier needs a numeric value"))
    for left in range(len(tier_ranges)):
        for right in range(left + 1, len(tier_ranges)):
            low_a, high_a, index_a = tier_ranges[left]
            low_b, high_b, index_b = tier_ranges[right]
            if max(low_a, low_b) <= min(high_a, high_b):
                issues.append(_issue("INVALID_SCORING_POLICY", f"profile.scoring.tiers[{index_b}]", f"overlaps tier {index_a}"))
def _packet_refs(profile: dict[str, Any], packet: dict[str, Any], issues: list[dict[str, str]]) -> None:
    evidence = _by_id(packet.get("evidence", []))
    evidence_ids = set(evidence)
    criterion_ids = set(_by_id(packet.get("criteria", [])))
    runtime_ids = set(_by_id(packet.get("runtime_results", [])))
    submission_artifact_ids = set(_by_id(packet.get("submission_artifacts", [])))
    automation_ids = set(_by_id(packet.get("automation_runs", [])))
    integrity_ids = set(_by_id(packet.get("integrity", {}).get("checks", [])))
    artifact_ids = set(_by_id(packet.get("artifacts", [])))
    crosscheck_ids = set(_by_id(packet.get("crosschecks", [])))
    authority_ids = set(_by_id(profile.get("authority_sources", [])))
    profile_criteria = set(_by_id(profile.get("criteria", [])))
    profile_artifact_types = set(_by_id(profile.get("artifact_types", [])))
    profile_obligations = set(_by_id(profile.get("runtime_obligations", [])))
    profile_roles = set(_by_id(profile.get("delegation_roles", [])))
    profile_templates = set(_by_id(profile.get("feedback_templates", [])))
    context = packet.get("student_context", {})
    for section in ("notes", "history"):
        for index, item in enumerate(context.get(section, [])):
            _refs(item.get("evidence_refs"), evidence_ids, "BROKEN_EVIDENCE_REF", f"packet.student_context.{section}[{index}].evidence_refs", issues)
    _refs(context.get("comparison", {}).get("evidence_refs"), evidence_ids, "BROKEN_EVIDENCE_REF", "packet.student_context.comparison.evidence_refs", issues)
    _refs(context.get("comparison", {}).get("crosscheck_refs", []), crosscheck_ids, "BROKEN_CROSSCHECK_REF", "packet.student_context.comparison.crosscheck_refs", issues)
    for index, run in enumerate(packet.get("automation_runs", [])):
        _refs(run.get("derived_evidence_refs"), evidence_ids, "BROKEN_EVIDENCE_REF", f"packet.automation_runs[{index}].derived_evidence_refs", issues)
        _refs([run.get("raw_evidence_ref")], evidence_ids, "BROKEN_EVIDENCE_REF", f"packet.automation_runs[{index}].raw_evidence_ref", issues)
    for index, item in enumerate(packet.get("evidence", [])):
        _refs(item.get("derived_from_refs"), evidence_ids, "BROKEN_EVIDENCE_REF", f"packet.evidence[{index}].derived_from_refs", issues)
    for index, criterion in enumerate(packet.get("criteria", [])):
        if criterion.get("profile_criterion_id") not in profile_criteria:
            issues.append(_issue("BROKEN_PROFILE_REF", f"packet.criteria[{index}].profile_criterion_id", "unknown profile criterion"))
        _refs(_authority_ids(criterion.get("rubric_refs")), authority_ids, "BROKEN_AUTHORITY_REF", f"packet.criteria[{index}].rubric_refs", issues)
        _refs(criterion.get("evidence_refs"), evidence_ids, "BROKEN_EVIDENCE_REF", f"packet.criteria[{index}].evidence_refs", issues)
        _refs(criterion.get("crosscheck_refs", []), crosscheck_ids, "BROKEN_CROSSCHECK_REF", f"packet.criteria[{index}].crosscheck_refs", issues)
    for index, result in enumerate(packet.get("runtime_results", [])):
        if result.get("profile_obligation_id") not in profile_obligations:
            issues.append(_issue("BROKEN_PROFILE_REF", f"packet.runtime_results[{index}].profile_obligation_id", "unknown runtime obligation"))
        _refs(result.get("evidence_refs"), evidence_ids, "BROKEN_EVIDENCE_REF", f"packet.runtime_results[{index}].evidence_refs", issues)
    for index, item in enumerate(packet.get("submission_artifacts", [])):
        if item.get("profile_artifact_type_id") not in profile_artifact_types:
            issues.append(_issue("BROKEN_PROFILE_REF", f"packet.submission_artifacts[{index}].profile_artifact_type_id", "unknown profile artifact type"))
        _refs(item.get("evidence_refs"), evidence_ids, "BROKEN_EVIDENCE_REF", f"packet.submission_artifacts[{index}].evidence_refs", issues)
        _refs(item.get("crosscheck_refs", []), crosscheck_ids, "BROKEN_CROSSCHECK_REF", f"packet.submission_artifacts[{index}].crosscheck_refs", issues)
    for index, assignment in enumerate(packet.get("capabilities", {}).get("role_assignments", [])):
        if assignment.get("profile_role_id") not in profile_roles:
            issues.append(_issue("BROKEN_PROFILE_REF", f"packet.capabilities.role_assignments[{index}].profile_role_id", "unknown delegation role"))
    for index, check in enumerate(packet.get("integrity", {}).get("checks", [])):
        _refs(check.get("evidence_refs"), evidence_ids, "BROKEN_EVIDENCE_REF", f"packet.integrity.checks[{index}].evidence_refs", issues)
    comparison_id = context.get("comparison", {}).get("id")
    known_subjects = criterion_ids | runtime_ids | integrity_ids | evidence_ids | submission_artifact_ids | automation_ids
    if comparison_id:
        known_subjects.add(comparison_id)
    for index, check in enumerate(packet.get("crosschecks", [])):
        if check.get("subject_ref") not in known_subjects:
            issues.append(_issue("BROKEN_SUBJECT_REF", f"packet.crosschecks[{index}].subject_ref", "unknown crosscheck subject"))
        _refs(_authority_ids(check.get("authority_refs")), authority_ids, "BROKEN_AUTHORITY_REF", f"packet.crosschecks[{index}].authority_refs", issues)
        _refs(check.get("evidence_refs"), evidence_ids, "BROKEN_EVIDENCE_REF", f"packet.crosschecks[{index}].evidence_refs", issues)
    for index, artifact in enumerate(packet.get("artifacts", [])):
        if artifact.get("template_id") is not None and artifact.get("template_id") not in profile_templates:
            issues.append(_issue("BROKEN_PROFILE_REF", f"packet.artifacts[{index}].template_id", "unknown feedback template"))
    decision = packet.get("decision", {})
    _refs(decision.get("criterion_refs"), criterion_ids, "BROKEN_CRITERION_REF", "packet.decision.criterion_refs", issues)
    _refs(decision.get("artifact_refs"), artifact_ids, "BROKEN_ARTIFACT_REF", "packet.decision.artifact_refs", issues)
    _refs(decision.get("scoring", {}).get("included_criterion_refs"), criterion_ids, "BROKEN_CRITERION_REF", "packet.decision.scoring.included_criterion_refs", issues)
def _profile_lock(profile: dict[str, Any], packet: dict[str, Any], issues: list[dict[str, str]]) -> None:
    lock = packet.get("profile", {})
    expected = {
        "profile_id": profile.get("profile_id"),
        "profile_version": profile.get("profile_version"),
        "schema_version": profile.get("schema_version"),
        "rubric_version": profile.get("course", {}).get("rubric_version"),
    }
    for field, value in expected.items():
        if lock.get(field) != value:
            issues.append(_issue("PROFILE_LOCK_MISMATCH", f"packet.profile.{field}", f"expected {value!r}"))
    if packet.get("schema_version") != profile.get("schema_version"):
        issues.append(_issue(
            "SCHEMA_VERSION_MISMATCH",
            "packet.schema_version",
            "packet and locked profile must use the same schema version",
        ))
    actual_sha = _canonical_sha256(profile)
    if lock.get("sha256", "").lower() != actual_sha:
        issues.append(_issue("PROFILE_SHA_MISMATCH", "packet.profile.sha256", f"expected canonical profile digest {actual_sha}"))
    if packet.get("submission", {}).get("course_id") != profile.get("course", {}).get("id"):
        issues.append(_issue("PROFILE_LOCK_MISMATCH", "packet.submission.course_id", "submission course differs from profile"))
def _evidence_semantics(packet: dict[str, Any], issues: list[dict[str, str]]) -> None:
    evidence = _by_id(packet.get("evidence", []))
    for index, item in enumerate(packet.get("evidence", [])):
        refs = item.get("derived_from_refs", [])
        _verify_local_digest(item.get("source_reference"), item.get("sha256"), f"packet.evidence[{index}].sha256", issues)
        if item.get("provenance") == "RAW" and refs:
            issues.append(_issue("RAW_EVIDENCE_HAS_PROVENANCE", f"packet.evidence[{index}].derived_from_refs", "raw evidence cannot derive from another item"))
        if item.get("provenance") == "DERIVED":
            if not refs or any(evidence.get(reference, {}).get("provenance") != "RAW" for reference in refs):
                issues.append(_issue("INVALID_DERIVED_PROVENANCE", f"packet.evidence[{index}].derived_from_refs", "derived evidence must reference raw evidence directly"))
            if packet.get("schema_version") == "1.1.0":
                source_id = item.get("source_evidence_id")
                if evidence.get(source_id, {}).get("provenance") != "RAW" or source_id not in refs:
                    issues.append(_issue(
                        "INVALID_SOURCE_EVIDENCE_ID",
                        f"packet.evidence[{index}].source_evidence_id",
                        "source_evidence_id must identify an available raw evidence item listed in derived_from_refs",
                    ))
        if item.get("kind") == "browser":
            browser_sources = [evidence.get(reference, {}) for reference in refs]
            if item.get("provenance") != "DERIVED" or not any(
                source.get("provenance") == "RAW" and source.get("kind") in {"browser", "runtime", "screenshot"}
                for source in browser_sources
            ):
                issues.append(_issue("INVALID_BROWSER_PROVENANCE", f"packet.evidence[{index}].derived_from_refs", "browser trace must derive from raw browser, runtime, or screenshot evidence"))
    crosschecks = packet.get("crosschecks", [])
    crosschecked = {
        item.get("subject_ref") for item in crosschecks
        if isinstance(item, dict) and item.get("status") in {"CONFIRMED", "OVERTURNED", "BLOCKED"}
    }
    final = packet.get("decision", {}).get("status") in FINAL_DECISIONS
    for index, run in enumerate(packet.get("automation_runs", [])):
        raw_output = run.get("raw_output", {})
        if raw_output.get("immutable") is not True:
            issues.append(_issue("RAW_AUTOMATION_NOT_IMMUTABLE", f"packet.automation_runs[{index}].raw_output", "raw automation output must remain immutable"))
        _verify_local_digest(raw_output.get("reference"), raw_output.get("sha256"), f"packet.automation_runs[{index}].raw_output.sha256", issues)
        raw_evidence = evidence.get(run.get("raw_evidence_ref"), {})
        if raw_evidence.get("provenance") != "RAW" or raw_evidence.get("sha256", "").lower() != str(raw_output.get("sha256", "")).lower():
            issues.append(_issue("AUTOMATION_RAW_LINK_MISMATCH", f"packet.automation_runs[{index}].raw_evidence_ref", "raw_evidence_ref must identify matching raw evidence"))
        for reference in run.get("derived_evidence_refs", []):
            if evidence.get(reference, {}).get("provenance") != "DERIVED":
                issues.append(_issue("INVALID_AUTOMATION_PROVENANCE", f"packet.automation_runs[{index}].derived_evidence_refs", "automation derived reference must identify derived evidence"))
        concerning = run.get("status") in {"FAILED", "ERROR"} or run.get("exit_code") is None or PLACEHOLDER_PATTERN.search(str(run.get("command", "")))
        if final and concerning and run.get("id") not in crosschecked:
            issues.append(_issue("AUTOMATION_CROSSCHECK_REQUIRED", f"packet.automation_runs[{index}]", "failed or incomplete automation metadata needs a resolved crosscheck before finalization"))
def _context_semantics(packet: dict[str, Any], issues: list[dict[str, str]]) -> None:
    comparison = packet.get("student_context", {}).get("comparison", {})
    if comparison.get("status") not in {"ANOMALY", "BLOCKED"}:
        return
    crosschecks = _by_id(packet.get("crosschecks", []))
    matching = [
        crosschecks[reference] for reference in comparison.get("crosscheck_refs", [])
        if reference in crosschecks and crosschecks[reference].get("subject_ref") == comparison.get("id")
    ]
    expected = "BLOCKED" if comparison.get("status") == "BLOCKED" else "CONFIRMED"
    if not any(item.get("status") == expected for item in matching):
        issues.append(_issue("HISTORY_CROSSCHECK_REQUIRED", "packet.student_context.comparison.crosscheck_refs", f"history comparison needs a matching {expected} crosscheck"))
def _submission_artifact_semantics(profile: dict[str, Any], packet: dict[str, Any], issues: list[dict[str, str]]) -> None:
    by_type: dict[str, list[dict[str, Any]]] = {}
    for item in packet.get("submission_artifacts", []):
        if isinstance(item, dict):
            by_type.setdefault(item.get("profile_artifact_type_id"), []).append(item)
    final = packet.get("decision", {}).get("status") in FINAL_DECISIONS
    crosschecks = _by_id(packet.get("crosschecks", []))
    for definition in profile.get("artifact_types", []):
        if not isinstance(definition, dict) or not definition.get("required"):
            continue
        matches = by_type.get(definition.get("id"), [])
        if len(matches) != 1:
            issues.append(_issue("REQUIRED_ARTIFACT_UNASSESSED", "packet.submission_artifacts", f"required artifact {definition.get('id')!r} needs exactly one assessment"))
        elif final and packet.get("decision", {}).get("status") == "LULUS" and matches[0].get("status") != "PRESENT":
            issues.append(_issue("REQUIRED_ARTIFACT_NOT_PRESENT", "packet.submission_artifacts", f"LULUS requires artifact {definition.get('id')!r} to be present"))
        if matches and matches[0].get("status") == "MISSING":
            item = matches[0]
            valid = [crosschecks[reference] for reference in item.get("crosscheck_refs", []) if reference in crosschecks and crosschecks[reference].get("subject_ref") == item.get("id")]
            if not any(check.get("status") == "CONFIRMED" for check in valid):
                issues.append(_issue("ARTIFACT_CROSSCHECK_REQUIRED", "packet.submission_artifacts", f"missing required artifact {definition.get('id')!r} needs a confirmed crosscheck"))
def _criteria_semantics(profile: dict[str, Any], packet: dict[str, Any], issues: list[dict[str, str]]) -> None:
    definitions = _by_id(profile.get("criteria", []))
    results = _by_id(packet.get("criteria", []))
    evidence = _by_id(packet.get("evidence", []))
    crosschecks = _by_id(packet.get("crosschecks", []))
    skip_rules = _by_id(profile.get("skip_rules", []))
    result_by_definition: dict[str, list[dict[str, Any]]] = {}
    for result in packet.get("criteria", []):
        if not isinstance(result, dict):
            continue
        result_by_definition.setdefault(result.get("profile_criterion_id"), []).append(result)
    if packet.get("decision", {}).get("status") in FINAL_DECISIONS:
        for definition_id in definitions:
            count = len(result_by_definition.get(definition_id, []))
            if count != 1:
                issues.append(_issue("CRITERION_COVERAGE_MISMATCH", "packet.criteria", f"profile criterion {definition_id!r} has {count} results"))
    for index, result in enumerate(packet.get("criteria", [])):
        if not isinstance(result, dict):
            continue
        definition = definitions.get(result.get("profile_criterion_id"))
        if not definition:
            continue
        for field in ("official_id", "title", "category"):
            if result.get(field) != definition.get(field):
                issues.append(_issue("CRITERION_DEFINITION_MISMATCH", f"packet.criteria[{index}].{field}", "result differs from locked profile"))
        if profile.get("schema_version") == "1.1.0" or packet.get("schema_version") == "1.1.0":
            if result.get("audience_labels") != definition.get("audience_labels"):
                issues.append(_issue(
                    "CRITERION_AUDIENCE_LABEL_MISMATCH",
                    f"packet.criteria[{index}].audience_labels",
                    "criterion audience labels differ from the locked profile",
                ))
        authorized_refs = {_canonical_sha256(item) for item in definition.get("authority_refs", []) if isinstance(item, dict)}
        result_refs = {_canonical_sha256(item) for item in result.get("rubric_refs", []) if isinstance(item, dict)}
        if not result_refs.issubset(authorized_refs):
            issues.append(_issue("CRITERION_AUTHORITY_MISMATCH", f"packet.criteria[{index}].rubric_refs", "rubric reference is not authorized for this criterion"))
        if (
            packet.get("schema_version") == "1.1.0"
            and result.get("status") not in {"UNREVIEWED", "BLOCKED"}
            and not any(
                reference.get("rule_role") in DECISIVE_RULE_ROLES
                and reference.get("outcome_effect") is True
                for reference in result.get("rubric_refs", [])
                if isinstance(reference, dict)
            )
        ):
            issues.append(_issue(
                "CRITERION_OUTCOME_BASIS_INVALID",
                f"packet.criteria[{index}].rubric_refs",
                "resolved criterion outcome needs an outcome-affecting official_gate or common_case rule",
            ))
        if packet.get("schema_version") == "1.1.0":
            for reference_index, reference in enumerate(result.get("rubric_refs", [])):
                _validate_rule_metadata(
                    reference,
                    f"packet.criteria[{index}].rubric_refs[{reference_index}]",
                    issues,
                )
        referenced = [evidence[ref] for ref in result.get("evidence_refs", []) if ref in evidence]
        evidence_kinds = {item.get("kind") for item in referenced}
        required_kinds = set(definition.get("required_evidence_kinds", []))
        if result.get("status") not in {"UNREVIEWED", "BLOCKED", "SKIPPED", "NOT_APPLICABLE"} and not required_kinds.issubset(evidence_kinds):
            issues.append(_issue("REQUIRED_EVIDENCE_KIND_MISSING", f"packet.criteria[{index}].evidence_refs", f"missing evidence kinds {sorted(required_kinds - evidence_kinds)!r}"))
        if result.get("category") == "mandatory" and result.get("status") in {"MET", "NOT_MET", "PARTIAL"}:
            only_automation = referenced and all(
                item.get("provenance") == "DERIVED" or item.get("kind") == "log"
                for item in referenced
            )
            if result.get("authority") == "AUTOMATION_SIGNAL" or only_automation:
                issues.append(_issue("AUTOGRADER_ONLY_DECISION", f"packet.criteria[{index}]", "mandatory decision cannot rely only on automation"))
        if result.get("status") in NEGATIVE_STATUSES:
            refs = result.get("crosscheck_refs", [])
            valid = [crosschecks[ref] for ref in refs if ref in crosschecks and crosschecks[ref].get("subject_ref") == result.get("id")]
            if not valid:
                issues.append(_issue("CROSSCHECK_REQUIRED", f"packet.criteria[{index}].crosscheck_refs", "negative or blocked criterion needs a matching crosscheck"))
            else:
                expected_status = "BLOCKED" if result.get("status") == "BLOCKED" else "CONFIRMED"
                if not any(check.get("status") == expected_status for check in valid):
                    issues.append(_issue("CROSSCHECK_STATUS_MISMATCH", f"packet.criteria[{index}].crosscheck_refs", f"criterion status requires a {expected_status} crosscheck"))
        if result.get("status") in {"SKIPPED", "NOT_APPLICABLE"}:
            rule = skip_rules.get(result.get("skip_rule_ref"))
            if rule is None:
                issues.append(_issue("SKIP_RULE_REQUIRED", f"packet.criteria[{index}].skip_rule_ref", "SKIPPED or NOT_APPLICABLE needs a profile-authorized skip rule"))
            elif rule.get("applies_to") != result.get("category"):
                issues.append(_issue("SKIP_RULE_MISMATCH", f"packet.criteria[{index}].skip_rule_ref", "skip rule does not apply to criterion category"))
            if not result.get("evidence_refs"):
                issues.append(_issue("SKIP_EVIDENCE_REQUIRED", f"packet.criteria[{index}].evidence_refs", "skipped or not-applicable criterion needs evidence"))
            if result.get("authority") != "AUTHORITATIVE_EXCEPTION":
                issues.append(_issue("SKIP_AUTHORITY_REQUIRED", f"packet.criteria[{index}].authority", "skipped or not-applicable criterion needs an authoritative exception"))
        if (
            packet.get("mode") == "FULL_REVIEW"
            and packet.get("decision", {}).get("status") in FINAL_DECISIONS
            and result.get("manual_owner", {}).get("owner_type") != "PARENT"
        ):
            issues.append(_issue("MANUAL_OWNER_REQUIRED", f"packet.criteria[{index}].manual_owner", "final full-review criterion must be validated by the parent reviewer"))
        points = result.get("points_awarded")
        allowed = definition.get("points")
        if points is not None and allowed is not None and points > allowed:
            issues.append(_issue("SCORING_POINTS_EXCEED_PROFILE", f"packet.criteria[{index}].points_awarded", "awarded points exceed profile points"))
    for index, check in enumerate(packet.get("crosschecks", [])):
        if check.get("status") == "UNRESOLVED" and packet.get("decision", {}).get("status") in FINAL_DECISIONS:
            issues.append(_issue("UNRESOLVED_CROSSCHECK", f"packet.crosschecks[{index}].status", "final decision cannot retain an unresolved crosscheck"))
def _runtime_semantics(profile: dict[str, Any], packet: dict[str, Any], issues: list[dict[str, str]]) -> None:
    results_by_obligation: dict[str, list[dict[str, Any]]] = {}
    for result in packet.get("runtime_results", []):
        results_by_obligation.setdefault(result.get("profile_obligation_id"), []).append(result)
    evidence = _by_id(packet.get("evidence", []))
    crosschecked_subjects = {
        item.get("subject_ref") for item in packet.get("crosschecks", [])
        if isinstance(item, dict)
    }
    final = packet.get("decision", {}).get("status") in FINAL_DECISIONS
    present_artifacts = {
        item.get("profile_artifact_type_id") for item in packet.get("submission_artifacts", [])
        if isinstance(item, dict) and item.get("status") == "PRESENT"
    }
    roles = _by_id(profile.get("delegation_roles", []))
    active_assignments = [
        item for item in packet.get("capabilities", {}).get("role_assignments", [])
        if isinstance(item, dict) and item.get("status") in {"DELEGATED", "PARENT_OWNED"}
    ]
    for obligation in profile.get("runtime_obligations", []):
        trigger = obligation.get("trigger", {})
        active = trigger.get("type") != "ARTIFACT_PRESENT" or trigger.get("artifact_type_ref") in present_artifacts
        if not obligation.get("required") or not active:
            continue
        matches = results_by_obligation.get(obligation.get("id"), [])
        if not matches:
            issues.append(_issue("MISSING_RUNTIME_RESULT", "packet.runtime_results", f"required obligation {obligation.get('id')!r} has no result"))
            continue
        if len(matches) > 1:
            issues.append(_issue("RUNTIME_RESULT_DUPLICATED", "packet.runtime_results", f"obligation {obligation.get('id')!r} has multiple results"))
        result = matches[0]
        owner_assignments = [item for item in active_assignments if item.get("owner_id") == result.get("owner_id")]
        if not owner_assignments:
            issues.append(_issue("RUNTIME_OWNER_MISMATCH", "packet.runtime_results", f"runtime owner {result.get('owner_id')!r} has no active role assignment"))
        compatible_capabilities = RUNTIME_ROLE_CAPABILITIES.get(obligation.get("kind"), set())
        compatible_assignments = [
            assignment for assignment in owner_assignments
            if roles.get(assignment.get("profile_role_id"), {}).get("capability") in compatible_capabilities
        ]
        if owner_assignments and not compatible_assignments:
            issues.append(_issue("RUNTIME_OWNER_CAPABILITY_MISMATCH", "packet.runtime_results", f"runtime owner {result.get('owner_id')!r} has no role compatible with {obligation.get('kind')!r}"))
        target = result.get("target_resource")
        owner_resources = {
            assignment.get("exclusive_resource") or roles.get(assignment.get("profile_role_id"), {}).get("exclusive_resource")
            for assignment in compatible_assignments
        }
        owner_resources.discard(None)
        if compatible_assignments and target not in owner_resources:
            issues.append(_issue("RUNTIME_TARGET_MISMATCH", "packet.runtime_results", f"runtime target {target!r} is not the compatible owner's exclusive resource"))
        if result.get("status") in {"FAILED", "ERROR"} and result.get("id") not in crosschecked_subjects:
            issues.append(_issue("CROSSCHECK_REQUIRED", "packet.crosschecks", f"failed runtime result {result.get('id')!r} needs a crosscheck"))
        if packet.get("decision", {}).get("status") == "LULUS" and result.get("status") != "PASSED":
            issues.append(_issue("REQUIRED_RUNTIME_NOT_PASSED", "packet.runtime_results", f"required obligation {obligation.get('id')!r} did not pass"))
        if obligation.get("browser_trace_required"):
            browser_items = [
                evidence[reference] for reference in result.get("evidence_refs", [])
                if reference in evidence and evidence[reference].get("kind") == "browser"
            ]
            complete = any(
                isinstance(item.get("browser_trace"), dict)
                and all(item["browser_trace"].get(field) for field in ("url", "control", "before", "action", "after", "conclusion"))
                for item in browser_items
            )
            if not complete:
                issues.append(_issue("BROWSER_TRACE_REQUIRED", "packet.runtime_results", f"obligation {obligation.get('id')!r} requires a complete browser trace"))
            for item in browser_items:
                trace = item.get("browser_trace", {})
                if trace.get("owner_id") != result.get("owner_id"):
                    issues.append(_issue("BROWSER_TRACE_OWNER_MISMATCH", "packet.runtime_results", f"browser trace owner must match runtime owner for {obligation.get('id')!r}"))
                if trace.get("target_resource") != target:
                    issues.append(_issue("BROWSER_TRACE_TARGET_MISMATCH", "packet.runtime_results", f"browser trace target must match runtime target for {obligation.get('id')!r}"))
def _capability_semantics(profile: dict[str, Any], packet: dict[str, Any], issues: list[dict[str, str]]) -> None:
    assignments = packet.get("capabilities", {}).get("role_assignments", [])
    multi_agent_available = any(
        item.get("kind") == "MULTI_AGENT" and item.get("status") == "AVAILABLE"
        for item in packet.get("capabilities", {}).get("items", []) if isinstance(item, dict)
    )
    by_role: dict[str, list[dict[str, Any]]] = {}
    for assignment in assignments:
        by_role.setdefault(assignment.get("profile_role_id"), []).append(assignment)
    if packet.get("mode") == "FULL_REVIEW":
        for role in profile.get("delegation_roles", []):
            if role.get("eligible_in_full_review") and len(by_role.get(role.get("id"), [])) != 1:
                issues.append(_issue("MISSING_ROLE_ASSIGNMENT", "packet.capabilities.role_assignments", f"eligible role {role.get('id')!r} needs exactly one assignment"))
    for index, assignment in enumerate(assignments):
        status = assignment.get("status")
        role = _by_id(profile.get("delegation_roles", [])).get(assignment.get("profile_role_id"), {})
        if packet.get("mode") == "FULL_REVIEW" and role.get("eligible_in_full_review") and status == "NOT_ELIGIBLE":
            issues.append(_issue("ROLE_ELIGIBILITY_MISMATCH", f"packet.capabilities.role_assignments[{index}].status", "eligible full-review role cannot be marked NOT_ELIGIBLE"))
        reason = str(assignment.get("capability_gate_reason", "")).strip()
        if status == "PARENT_OWNED" and not reason:
            issues.append(_issue("PARENT_OWNED_REASON_REQUIRED", f"packet.capabilities.role_assignments[{index}]", "parent-owned role needs a capability-gate reason"))
        if status == "BLOCKED" and not reason:
            issues.append(_issue("ROLE_BLOCK_REASON_REQUIRED", f"packet.capabilities.role_assignments[{index}]", "blocked role needs a capability-gate reason"))
        expected_owner = {"PARENT_OWNED": "PARENT", "DELEGATED": "SUBAGENT"}.get(status)
        if expected_owner and assignment.get("owner_type") != expected_owner:
            issues.append(_issue("INVALID_ROLE_OWNER", f"packet.capabilities.role_assignments[{index}].owner_type", f"{status} requires {expected_owner}"))
        if (
            packet.get("mode") == "FULL_REVIEW"
            and packet.get("decision", {}).get("status") in FINAL_DECISIONS
            and multi_agent_available
            and role.get("eligible_in_full_review")
            and status != "DELEGATED"
        ):
            non_isolation = assignment.get("non_isolation_reason")
            allowed = False
            if status == "PARENT_OWNED" and isinstance(non_isolation, dict):
                if non_isolation.get("kind") == "EXCLUSIVE_RESOURCE":
                    resource = assignment.get("exclusive_resource") or role.get("exclusive_resource")
                    allowed = bool(resource) and non_isolation.get("exclusive_resource") == resource
                elif non_isolation.get("kind") == "CAPABILITY_CONSTRAINT":
                    capability_kind = ROLE_CAPABILITY_ITEM_KINDS.get(role.get("capability"))
                    matching_capabilities = [
                        item for item in packet.get("capabilities", {}).get("items", [])
                        if isinstance(item, dict) and item.get("kind") == capability_kind
                    ]
                    allowed = (
                        non_isolation.get("capability") == role.get("capability")
                        and bool(matching_capabilities)
                        and all(
                            item.get("status") == "UNAVAILABLE"
                            and str(item.get("reason", "")).strip()
                            for item in matching_capabilities
                        )
                    )
            if not allowed:
                issues.append(_issue("DELEGATION_REQUIRED", f"packet.capabilities.role_assignments[{index}]", "available multi-agent capability requires delegation unless a structured exclusive-resource or capability constraint prevents isolation"))
    resources: dict[str, str] = {}
    roles = _by_id(profile.get("delegation_roles", []))
    for index, assignment in enumerate(assignments):
        profile_resource = roles.get(assignment.get("profile_role_id"), {}).get("exclusive_resource")
        resource = assignment.get("exclusive_resource") or profile_resource
        if not resource or assignment.get("owner_type") == "NONE":
            continue
        owner = assignment.get("owner_id")
        if resource in resources and resources[resource] != owner:
            issues.append(_issue("EXCLUSIVE_RESOURCE_CONFLICT", f"packet.capabilities.role_assignments[{index}].exclusive_resource", f"resource already owned by {resources[resource]!r}"))
        else:
            resources[resource] = owner
def _scoring_semantics(profile: dict[str, Any], packet: dict[str, Any], issues: list[dict[str, str]]) -> None:
    policy = profile.get("scoring", {})
    scoring = packet.get("decision", {}).get("scoring", {})
    if scoring.get("mode") != policy.get("mode"):
        issues.append(_issue("SCORING_MODE_MISMATCH", "packet.decision.scoring.mode", "scoring mode differs from profile"))
        return
    results = _by_id(packet.get("criteria", []))
    definitions = _by_id(profile.get("criteria", []))
    mandatory_pass = all(
        result.get("status") in PASSING_MANDATORY
        for result in packet.get("criteria", []) if result.get("category") == "mandatory"
    ) and any(result.get("category") == "mandatory" for result in packet.get("criteria", []))
    if policy.get("optional_scoring_requires_mandatory_pass") and not mandatory_pass and scoring.get("eligible"):
        issues.append(_issue("SCORING_NOT_ELIGIBLE", "packet.decision.scoring.eligible", "mandatory criteria have not all passed"))
    included = scoring.get("included_criterion_refs", [])
    if policy.get("mode") == "optional_count":
        invalid_refs = [
            reference
            for reference in included
            if results.get(reference, {}).get("category") != "optional"
        ]
        if invalid_refs:
            issues.append(
                _issue(
                    "INVALID_SCORING_REF",
                    "packet.decision.scoring.included_criterion_refs",
                    f"non-optional refs included: {invalid_refs!r}",
                )
            )
    if not scoring.get("eligible"):
        if scoring.get("value") is not None or scoring.get("maximum") is not None or included:
            issues.append(_issue("INELIGIBLE_SCORING_HAS_VALUE", "packet.decision.scoring", "ineligible scoring must have null values and no included refs"))
        return
    if policy.get("mode") == "none":
        if scoring.get("value") is not None or included:
            issues.append(_issue("SCORING_VALUE_MISMATCH", "packet.decision.scoring", "mode none cannot produce a score"))
        return
    if scoring.get("maximum") != policy.get("maximum_score"):
        issues.append(_issue("SCORING_MAXIMUM_MISMATCH", "packet.decision.scoring.maximum", "maximum differs from profile"))
    if policy.get("mode") == "optional_count":
        expected_refs = sorted(
            result_id
            for result_id, result in results.items()
            if result.get("category") == "optional"
            and result.get("status") in {"MET", "NOT_MET", "PARTIAL"}
        )
    else:
        expected_refs = sorted(
            result_id
            for result_id, result in results.items()
            if result.get("status") in {"MET", "NOT_MET", "PARTIAL"}
            and "points" in definitions.get(result.get("profile_criterion_id"), {})
        )
    if policy.get("mode") == "optional_count":
        met_count = sum(results[reference].get("status") == "MET" for reference in expected_refs)
        tiers = [tier for tier in policy.get("tiers", []) if tier.get("minimum", math.inf) <= met_count <= tier.get("maximum", -math.inf)]
        if len(tiers) != 1 or not isinstance(tiers[0].get("value") if tiers else None, (int, float)):
            issues.append(_issue("INVALID_SCORING_POLICY", "profile.scoring.tiers", f"optional count {met_count} must match exactly one valued tier"))
            expected_value = math.nan
        else:
            expected_value = tiers[0]["value"]
    else:
        missing_points = [reference for reference in expected_refs if results[reference].get("points_awarded") is None]
        if missing_points:
            issues.append(_issue("SCORING_POINTS_REQUIRED", "packet.criteria", f"scored results need points: {missing_points!r}"))
        values = [results[reference].get("points_awarded", 0) for reference in expected_refs]
        expected_value = sum(values) / len(values) if values else policy.get("base_score", 0)
        expected_value = min(policy.get("maximum_score", math.inf), expected_value)
        for reference in expected_refs:
            definition = definitions.get(results[reference].get("profile_criterion_id"), {})
            if "points" not in definition:
                issues.append(_issue("INVALID_SCORING_REF", "packet.decision.scoring.included_criterion_refs", f"criterion {reference!r} has no profile points"))
    if sorted(included) != expected_refs:
        issues.append(_issue("SCORING_INCLUDED_REFS_MISMATCH", "packet.decision.scoring.included_criterion_refs", f"expected {expected_refs!r}"))
    value = scoring.get("value")
    if not isinstance(value, (int, float)) or isinstance(value, bool) or not math.isclose(float(value), float(expected_value), rel_tol=1e-9, abs_tol=1e-9):
        issues.append(_issue("SCORING_VALUE_MISMATCH", "packet.decision.scoring.value", f"expected {expected_value!r}"))
def _artifact_text(reference: Any) -> str | None:
    if not isinstance(reference, str):
        return None
    value = reference[7:] if reference.startswith("file://") else reference
    path = Path(value)
    if not path.is_absolute() or not path.is_file():
        return None
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError):
        return None
def _artifacts_and_decision(profile: dict[str, Any], packet: dict[str, Any], issues: list[dict[str, str]]) -> None:
    artifacts = _by_id(packet.get("artifacts", []))
    templates = _by_id(profile.get("feedback_templates", []))
    decision = packet.get("decision", {})
    status = decision.get("status")
    if status == "PENDING":
        issues.append(_issue("FINAL_DECISION_REQUIRED", "packet.decision.status", "auditor cannot report READY for a pending decision"))
    if status in FINAL_DECISIONS:
        expected_criteria = set(_by_id(packet.get("criteria", [])))
        if set(decision.get("criterion_refs", [])) != expected_criteria:
            issues.append(_issue("DECISION_CRITERION_REFS_MISMATCH", "packet.decision.criterion_refs", "final decision must reference every criterion result exactly once"))
    selected = [artifacts[ref] for ref in decision.get("artifact_refs", []) if ref in artifacts]
    expected_kind = {"LULUS": "APPROVAL_LETTER", "REVISI": "REJECTION_LETTER"}.get(status)
    if expected_kind and not any(
        item.get("kind") == expected_kind and item.get("audience") == "STUDENT" and item.get("status") == "FINAL" and item.get("decision_label") == status
        for item in selected
    ):
        issues.append(_issue("DECISION_ARTIFACT_MISMATCH", "packet.decision.artifact_refs", f"{status} needs a matching final {expected_kind}"))
    for index, artifact in enumerate(packet.get("artifacts", [])):
        _verify_local_digest(artifact.get("reference"), artifact.get("sha256"), f"packet.artifacts[{index}].sha256", issues)
        if artifact.get("decision_label") and artifact.get("status") == "FINAL" and artifact.get("id") in decision.get("artifact_refs", []) and artifact.get("decision_label") != status:
            issues.append(_issue("DECISION_ARTIFACT_MISMATCH", f"packet.artifacts[{index}].decision_label", "artifact decision label differs from packet"))
        if artifact.get("audience") != "STUDENT" or artifact.get("status") != "FINAL":
            continue
        local = _local_path(artifact.get("reference"))
        if local is None or not local.is_file():
            issues.append(_issue("STUDENT_ARTIFACT_UNREADABLE", f"packet.artifacts[{index}].reference", "final student artifact must be an absolute readable local file"))
            continue
        content = _artifact_text(artifact.get("reference"))
        if content is None:
            issues.append(_issue("STUDENT_ARTIFACT_UNREADABLE", f"packet.artifacts[{index}].reference", "final student artifact could not be decoded as UTF-8"))
            continue
        if _contains_absolute_posix_path(content) or re.search(r"(?:[A-Za-z]:\\Users\\|internal://|reviewer[-_ ]?(?:log|note))", content, re.IGNORECASE):
            issues.append(_issue("UNSAFE_STUDENT_CONTENT", f"packet.artifacts[{index}].reference", "student-facing content exposes an internal path or reviewer detail"))
        if PLACEHOLDER_PATTERN.search(content) or re.search(r"\[(?:NAMA|NAME|INSERT)[^]]*\]", content, re.IGNORECASE):
            issues.append(_issue("PLACEHOLDER_IN_STUDENT_CONTENT", f"packet.artifacts[{index}].reference", "student-facing content contains an unresolved placeholder"))
        if re.search(r"(?:\b(?:api[_-]?key|access[_-]?token|password|secret)\b\s*[:=]\s*\S{8,}|\bsk-[A-Za-z0-9_-]{12,})", content, re.IGNORECASE):
            issues.append(_issue("SECRET_IN_STUDENT_CONTENT", f"packet.artifacts[{index}].reference", "student-facing content contains a possible unredacted secret"))
        if artifact.get("id") in decision.get("artifact_refs", []) and status in FINAL_DECISIONS:
            template = templates.get(artifact.get("template_id"), {})
            if template.get("decision") != status:
                issues.append(_issue("TEMPLATE_DECISION_MISMATCH", f"packet.artifacts[{index}].template_id", "selected template decision differs from packet"))
            if template.get("audience", "student") != "student":
                issues.append(_issue("TEMPLATE_AUDIENCE_MISMATCH", f"packet.artifacts[{index}].template_id", "student artifact cannot use an internal template"))
            if re.search(rf"\bstatus\s*:\s*{re.escape(str(status))}\b", content, re.IGNORECASE) is None:
                issues.append(_issue("CONTENT_DECISION_MISMATCH", f"packet.artifacts[{index}].reference", "student artifact content does not state the packet decision"))
            scoring = decision.get("scoring", {})
            if scoring.get("eligible"):
                value = scoring.get("value")
                if artifact.get("rating_value") != value:
                    issues.append(_issue("ARTIFACT_RATING_MISMATCH", f"packet.artifacts[{index}].rating_value", "artifact rating differs from packet scoring"))
                label = format(float(value), "g") if isinstance(value, (int, float)) and not isinstance(value, bool) else ""
                if not label or re.search(rf"\b(?:rating|bintang|score|nilai)\s*:\s*{re.escape(label)}(?:\.0+)?\b", content, re.IGNORECASE) is None:
                    issues.append(_issue("CONTENT_RATING_MISMATCH", f"packet.artifacts[{index}].reference", "student artifact content does not state the packet rating"))
    mandatory = [item for item in packet.get("criteria", []) if isinstance(item, dict) and item.get("category") == "mandatory"]
    if status == "LULUS" and (not mandatory or any(item.get("status") not in PASSING_MANDATORY for item in mandatory)):
        issues.append(_issue("DECISION_CRITERIA_MISMATCH", "packet.decision.status", "LULUS requires all mandatory criteria to pass"))
    if status == "REVISI" and not any(item.get("status") in {"NOT_MET", "PARTIAL"} for item in mandatory):
        issues.append(_issue("DECISION_CRITERIA_MISMATCH", "packet.decision.status", "REVISI requires a confirmed mandatory non-pass result"))
    audit_data = packet.get("audit", {})
    signoff = audit_data.get("manual_signoff", {})
    if status in FINAL_DECISIONS:
        if audit_data.get("workflow_state") != "READY":
            issues.append(_issue("WORKFLOW_NOT_READY", "packet.audit.workflow_state", "final decision requires READY workflow"))
        if signoff.get("signed") is not True or not signoff.get("signed_at"):
            issues.append(_issue("MANUAL_SIGNOFF_REQUIRED", "packet.audit.manual_signoff", "final decision requires dated manual sign-off"))
        runs = audit_data.get("validator_runs", [])
        if not runs or runs[-1].get("result") != "READY" or runs[-1].get("issue_count") != 0:
            issues.append(_issue("FRESH_VALIDATOR_RESULT_REQUIRED", "packet.audit.validator_runs", "final decision requires a recorded successful validator run"))
        elif runs:
            latest = runs[-1]
            if latest.get("tool_version") != VERSION:
                issues.append(_issue("VALIDATOR_VERSION_MISMATCH", "packet.audit.validator_runs", f"expected auditor version {VERSION}"))
            if latest.get("profile_sha256", "").lower() != _canonical_sha256(profile):
                issues.append(_issue("VALIDATOR_PROFILE_HASH_MISMATCH", "packet.audit.validator_runs", "validator run does not bind the current profile"))
            if latest.get("packet_sha256", "").lower() != _packet_subject_sha256(packet):
                issues.append(_issue("VALIDATOR_PACKET_HASH_MISMATCH", "packet.audit.validator_runs", "validator run does not bind the current packet"))
            command = str(latest.get("command", ""))
            if PLACEHOLDER_PATTERN.search(command) or not all(token in command for token in ("audit_review.py", "--profile", "--packet")):
                issues.append(_issue("VALIDATOR_COMMAND_INVALID", "packet.audit.validator_runs", "validator run needs the concrete auditor command"))
def _blocked_issues(profile: dict[str, Any], packet: dict[str, Any]) -> list[dict[str, str]]:
    blockers: list[dict[str, str]] = []
    relevant = set()
    if packet.get("mode") == "FULL_REVIEW" and any(role.get("eligible_in_full_review") for role in profile.get("delegation_roles", [])):
        relevant.add("MULTI_AGENT")
    runtime_capability = {
        "notebook_execution": "PYTHON_RUNTIME", "source_execution": "PYTHON_RUNTIME", "test_suite": "PYTHON_RUNTIME",
        "browser_interaction": "BROWSER", "http_probe": "NETWORK", "repository_access": "NETWORK", "external_service": "EXTERNAL_SERVICE",
    }
    present = {item.get("profile_artifact_type_id") for item in packet.get("submission_artifacts", []) if item.get("status") == "PRESENT"}
    for obligation in profile.get("runtime_obligations", []):
        trigger = obligation.get("trigger", {})
        active = trigger.get("type") != "ARTIFACT_PRESENT" or trigger.get("artifact_type_ref") in present
        if obligation.get("required") and active and runtime_capability.get(obligation.get("kind")):
            relevant.add(runtime_capability[obligation["kind"]])
    for index, capability in enumerate(packet.get("capabilities", {}).get("items", [])):
        if capability.get("status") == "UNAVAILABLE":
            if not str(capability.get("reason", "")).strip():
                blockers.append(_issue("CAPABILITY_REASON_REQUIRED", f"packet.capabilities.items[{index}].reason", "unavailable capability needs a reason"))
            elif capability.get("kind") in relevant:
                blockers.append(_issue("CAPABILITY_BLOCKED", f"packet.capabilities.items[{index}]", capability["reason"], "BLOCKED"))
    for index, assignment in enumerate(packet.get("capabilities", {}).get("role_assignments", [])):
        if isinstance(assignment, dict) and assignment.get("status") == "BLOCKED":
            blockers.append(_issue("ROLE_BLOCKED", f"packet.capabilities.role_assignments[{index}]", assignment.get("capability_gate_reason", "blocked role"), "BLOCKED"))
    for section, items in (
        ("criteria", packet.get("criteria", [])),
        ("runtime_results", packet.get("runtime_results", [])),
        ("crosschecks", packet.get("crosschecks", [])),
    ):
        for index, item in enumerate(items):
            if not isinstance(item, dict):
                continue
            if item.get("status") == "BLOCKED":
                blockers.append(_issue("REVIEW_GATE_BLOCKED", f"packet.{section}[{index}]", item.get("conclusion", "blocked"), "BLOCKED"))
    for index, item in enumerate(packet.get("integrity", {}).get("checks", [])):
        if not isinstance(item, dict):
            continue
        if item.get("status") == "BLOCKED":
            blockers.append(_issue("REVIEW_GATE_BLOCKED", f"packet.integrity.checks[{index}]", item.get("conclusion", "blocked"), "BLOCKED"))
    for index, item in enumerate(packet.get("submission_artifacts", [])):
        if isinstance(item, dict) and item.get("status") == "BLOCKED":
            blockers.append(_issue("REVIEW_GATE_BLOCKED", f"packet.submission_artifacts[{index}]", item.get("conclusion", "blocked"), "BLOCKED"))
    if packet.get("decision", {}).get("status") == "BLOCKED" or packet.get("audit", {}).get("workflow_state") == "BLOCKED":
        reason = str(packet.get("audit", {}).get("blocked_reason", "")).strip()
        if not reason:
            blockers.append(_issue("BLOCKED_REASON_REQUIRED", "packet.audit.blocked_reason", "blocked review needs a reason"))
        else:
            blockers.append(_issue("DECISION_BLOCKED", "packet.decision.status", reason, "BLOCKED"))
    return blockers
def audit(profile: dict[str, Any], packet: dict[str, Any]) -> dict[str, Any]:
    """Return deterministic status, exit code, and stable issue records."""
    try:
        profile_schema = json.loads(PROFILE_SCHEMA_PATH.read_text(encoding="utf-8"))
        packet_schema = json.loads(PACKET_SCHEMA_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        issue = _issue("AUDITOR_CONFIGURATION_ERROR", "$", str(error))
        return {"status": "INVALID", "exit_code": 1, "issues": [issue]}
    issues = SchemaValidator(profile_schema, "profile").validate(profile)
    issues.extend(SchemaValidator(packet_schema, "packet").validate(packet))
    fatal_shape_error = any(
        item["code"] == "SCHEMA_VALIDATION" and "expected type" in item["message"]
        for item in issues
    )
    if isinstance(profile, dict) and isinstance(packet, dict) and not fatal_shape_error:
        for path, items in _arrays(packet, "packet"):
            _check_unique(items, path, issues)
        _profile_semantics(profile, issues)
        _packet_refs(profile, packet, issues)
        _profile_lock(profile, packet, issues)
        _evidence_semantics(packet, issues)
        _context_semantics(packet, issues)
        _submission_artifact_semantics(profile, packet, issues)
        _criteria_semantics(profile, packet, issues)
        _runtime_semantics(profile, packet, issues)
        _capability_semantics(profile, packet, issues)
        _scoring_semantics(profile, packet, issues)
        _artifacts_and_decision(profile, packet, issues)
        crosschecked_subjects = {
            item.get("subject_ref") for item in packet.get("crosschecks", [])
            if isinstance(item, dict)
        }
        for index, check in enumerate(packet.get("integrity", {}).get("checks", [])):
            if isinstance(check, dict) and check.get("status") == "FLAGGED" and check.get("id") not in crosschecked_subjects:
                issues.append(_issue("CROSSCHECK_REQUIRED", f"packet.integrity.checks[{index}]", "flagged integrity finding needs a crosscheck"))
    blockers = _blocked_issues(profile, packet) if isinstance(profile, dict) and isinstance(packet, dict) else []
    errors = [item for item in issues + blockers if item["severity"] == "ERROR"]
    if errors:
        status, exit_code = "INVALID", 1
        combined = issues + blockers
    elif blockers:
        status, exit_code = "BLOCKED", 2
        combined = blockers
    else:
        status, exit_code, combined = "READY", 0, []
    combined.sort(key=lambda item: (item["code"], item["path"], item["message"], item["severity"]))
    return {"status": status, "exit_code": exit_code, "issues": combined}
def _load_json(path: Path, label: str) -> tuple[dict[str, Any] | None, dict[str, Any] | None]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return None, {
            "status": "INVALID", "exit_code": 1,
            "issues": [_issue("INPUT_JSON_ERROR", label, str(error))],
        }
    if not isinstance(value, dict):
        return None, {
            "status": "INVALID", "exit_code": 1,
            "issues": [_issue("INPUT_JSON_ERROR", label, "top-level JSON value must be an object")],
        }
    return value, None
def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", required=True, type=Path)
    parser.add_argument("--packet", required=True, type=Path)
    args = parser.parse_args(argv)
    profile, error = _load_json(args.profile, "profile")
    if error:
        print(json.dumps(error, indent=2, sort_keys=True))
        return 1
    packet, error = _load_json(args.packet, "packet")
    if error:
        print(json.dumps(error, indent=2, sort_keys=True))
        return 1
    result = audit(profile, packet)
    print(json.dumps(result, indent=2, sort_keys=True))
    return result["exit_code"]
if __name__ == "__main__":
    sys.exit(main())
