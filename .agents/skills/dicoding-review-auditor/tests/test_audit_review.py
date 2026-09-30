import copy
import hashlib
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
AUDITOR_PATH = ROOT / "scripts" / "audit_review.py"
H = "a" * 64
T = "2026-09-12T05:00:00Z"
FIXTURE_DIRECTORY = tempfile.TemporaryDirectory()
APPROVAL_CONTENT = "Status: LULUS\nRating: 4\nSemua kriteria wajib terpenuhi.\n"
APPROVAL_PATH = Path(FIXTURE_DIRECTORY.name) / "approval.md"
APPROVAL_PATH.write_text(APPROVAL_CONTENT, encoding="utf-8")
REJECTION_CONTENT = "Status: REVISI\nKriteria wajib masih terpenuhi sebagian.\n"
REJECTION_PATH = Path(FIXTURE_DIRECTORY.name) / "rejection.md"
REJECTION_PATH.write_text(REJECTION_CONTENT, encoding="utf-8")


def digest_bytes(value):
    return hashlib.sha256(value).hexdigest()


def canonical_digest(value):
    payload = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return digest_bytes(payload.encode("utf-8"))


def file_digest(path):
    return digest_bytes(Path(path).read_bytes())


def rubric_ref(locator="criteria"):
    return {"authority_id": "rubric", "locator": locator}


def rule_ref(
    locator="criteria",
    role="official_gate",
    outcome_effect=True,
    authority_class="AUTHORITATIVE_REQUIREMENT",
):
    return {
        "authority_id": "rubric",
        "locator": locator,
        "rule_role": role,
        "authority_class": authority_class,
        "outcome_effect": outcome_effect,
    }


def packet_subject_digest(packet):
    subject = copy.deepcopy(packet)
    subject.get("audit", {})["validator_runs"] = []
    return canonical_digest(subject)


def seal_packet(profile, packet):
    packet["profile"]["sha256"] = canonical_digest(profile)
    run = packet["audit"]["validator_runs"][-1]
    run.update({
        "tool_version": "1.1.0",
        "profile_sha256": canonical_digest(profile),
        "command": "python3 scripts/audit_review.py --profile profile.json --packet packet.json",
        "result": "READY",
        "issue_count": 0,
    })
    run["packet_sha256"] = packet_subject_digest(packet)
    return packet


def evidence(evidence_id, order, kind, provenance="RAW", derived_from=None, trace=None):
    item = {
        "id": evidence_id,
        "order": order,
        "kind": kind,
        "provenance": provenance,
        "source_reference": f"intake://{evidence_id}",
        "captured_at": T,
        "sha256": H,
        "summary": f"Evidence {evidence_id}",
        "derived_from_refs": derived_from or [],
    }
    if trace is not None:
        item["browser_trace"] = trace
    return item


def dashboard_profile():
    return {
        "schema_version": "1.0.0",
        "profile_id": "badp-dashboard",
        "profile_version": "1.0.0",
        "course": {
            "id": "badp",
            "title": "Analisis Data",
            "project_title": "Dashboard Analisis",
            "rubric_version": "2.0.0",
        },
        "authority_sources": [
            {"id": "rubric", "order": 0, "kind": "official_rubric", "title": "Rubrik", "reference": "official://rubric", "applicability": "ACTIVE"},
            {"id": "common-cases", "order": 1, "kind": "official_common_case", "title": "Kasus umum", "reference": "official://common-cases", "applicability": "ACTIVE"},
            {"id": "class-skill", "order": 2, "kind": "class_skill", "title": "Skill kelas", "reference": "skill://badp", "applicability": "ACTIVE"},
            {"id": "feedback-standard", "order": 3, "kind": "feedback_standard", "title": "Standar feedback", "reference": "official://feedback", "applicability": "ACTIVE"},
            {"id": "autograder", "order": 4, "kind": "automation", "title": "Autograder", "reference": "script://review", "applicability": "ACTIVE"},
        ],
        "artifact_types": [
            {"id": "notebook", "order": 0, "title": "Notebook", "required": True, "accepted_extensions": [".ipynb"]},
            {"id": "dashboard", "order": 1, "title": "Dashboard", "required": True, "accepted_extensions": [".py"]},
        ],
        "criteria": [
            {
                "id": "mandatory-analysis", "order": 0, "official_id": "K1", "title": "Analisis lengkap",
                "category": "mandatory", "authority_refs": [rubric_ref("K1")], "required_evidence_kinds": ["notebook"],
                "manual_review_required": True, "runtime_obligation_refs": [],
            },
            {
                "id": "mandatory-dashboard", "order": 1, "official_id": "K2", "title": "Dashboard interaktif",
                "category": "mandatory", "authority_refs": [rubric_ref("K2")], "required_evidence_kinds": ["source", "browser"],
                "manual_review_required": True, "runtime_obligation_refs": ["dashboard-browser"],
            },
            {
                "id": "optional-documentation", "order": 2, "official_id": "S1", "title": "Dokumentasi",
                "category": "optional", "authority_refs": [rubric_ref("S1")], "required_evidence_kinds": ["file"],
                "manual_review_required": True, "runtime_obligation_refs": [], "points": 1,
            },
            {
                "id": "optional-analysis", "order": 3, "official_id": "S2", "title": "Analisis lanjutan",
                "category": "optional", "authority_refs": [rubric_ref("S2")], "required_evidence_kinds": ["notebook"],
                "manual_review_required": True, "runtime_obligation_refs": [], "points": 1,
            },
        ],
        "runtime_obligations": [
            {"id": "dashboard-browser", "order": 0, "title": "Interaksi dashboard", "kind": "browser_interaction", "required": True, "browser_trace_required": True, "trigger": {"type": "ALWAYS"}}
        ],
        "scoring": {
            "mode": "optional_count", "optional_scoring_requires_mandatory_pass": True,
            "base_score": 2, "maximum_score": 4,
            "tiers": [
                {"id": "two-star", "order": 0, "label": "Dua", "minimum": 0, "maximum": 0, "value": 2},
                {"id": "three-star", "order": 1, "label": "Tiga", "minimum": 1, "maximum": 1, "value": 3},
                {"id": "four-star", "order": 2, "label": "Empat", "minimum": 2, "maximum": 2, "value": 4},
            ],
        },
        "delegation_roles": [
            {"id": "files", "order": 0, "title": "Artefak", "capability": "filesystem", "eligible_in_full_review": True},
            {"id": "browser", "order": 1, "title": "Browser", "capability": "browser", "eligible_in_full_review": True, "exclusive_resource": "browser:9222"},
        ],
        "feedback_templates": [
            {"id": "approval", "order": 0, "decision": "LULUS", "reference": "template://approval", "audience": "student"},
            {"id": "rejection", "order": 1, "decision": "REVISI", "reference": "template://rejection", "audience": "student"},
            {"id": "blocked", "order": 2, "decision": "BLOCKED", "reference": "template://blocked", "audience": "internal"},
        ],
        "skip_rules": [],
    }


def dashboard_packet():
    profile = dashboard_profile()
    trace = {
        "url": "https://example.test/dashboard", "control": "Filter musim", "before": "Semua musim",
        "action": "Pilih Spring", "after": "Grafik menampilkan Spring", "conclusion": "Filter berfungsi",
        "owner_id": "agent-browser", "target_resource": "browser:9222",
    }
    packet = {
        "schema_version": "1.0.0", "review_id": "review-5027475", "mode": "FULL_REVIEW",
        "submission": {
            "id": "submission-5027475", "course_id": "badp", "tutorial_id": "project",
            "submitter_id": "student-1", "source_reference": "submission://5027475", "submitted_at": T, "intake_sha256": H,
        },
        "profile": {
            "profile_id": "badp-dashboard", "profile_version": "1.0.0", "schema_version": "1.0.0",
            "rubric_version": "2.0.0", "sha256": canonical_digest(profile), "locked_at": T,
        },
        "student_context": {
            "notes": [{"id": "note-1", "order": 0, "raw_text": "thank u", "classification": "ACKNOWLEDGEMENT", "evidence_refs": ["ev-note"]}],
            "history_status": "NEW", "history": [],
            "comparison": {"id": "context-comparison", "status": "NOT_APPLICABLE", "summary": "Submission pertama", "evidence_refs": ["ev-note"], "crosscheck_refs": []},
        },
        "capabilities": {
            "items": [
                {"id": "cap-agent", "order": 0, "kind": "MULTI_AGENT", "status": "AVAILABLE"},
                {"id": "cap-browser", "order": 1, "kind": "BROWSER", "status": "AVAILABLE"},
            ],
            "role_assignments": [
                {"id": "role-files", "order": 0, "profile_role_id": "files", "status": "DELEGATED", "owner_type": "SUBAGENT", "owner_id": "agent-files"},
                {"id": "role-browser", "order": 1, "profile_role_id": "browser", "status": "DELEGATED", "owner_type": "SUBAGENT", "owner_id": "agent-browser", "exclusive_resource": "browser:9222"},
            ],
        },
        "automation_runs": [{
            "id": "run-1", "order": 0, "tool": "main-review", "tool_version": "1.0.0", "status": "PASSED", "started_at": T, "finished_at": T,
            "command": "python main_review.py", "exit_code": 0,
            "raw_output": {"reference": "evidence://raw-autograder", "sha256": H, "immutable": True, "media_type": "text/plain"},
            "raw_evidence_ref": "ev-auto-raw",
            "derived_evidence_refs": ["ev-auto-derived"],
        }],
        "evidence": [
            evidence("ev-note", 0, "student_note"),
            evidence("ev-notebook", 1, "notebook"),
            evidence("ev-source", 2, "source"),
            evidence("ev-browser-raw", 3, "screenshot"),
            evidence("ev-browser", 4, "browser", "DERIVED", ["ev-browser-raw"], trace),
            evidence("ev-readme", 5, "file"),
            evidence("ev-auto-raw", 6, "log"),
            evidence("ev-auto-derived", 7, "log", "DERIVED", ["ev-auto-raw"]),
            evidence("ev-integrity", 8, "source"),
        ],
        "submission_artifacts": [
            {"id": "submitted-notebook", "order": 0, "profile_artifact_type_id": "notebook", "status": "PRESENT", "evidence_refs": ["ev-notebook"], "conclusion": "Notebook tersedia"},
            {"id": "submitted-dashboard", "order": 1, "profile_artifact_type_id": "dashboard", "status": "PRESENT", "evidence_refs": ["ev-source"], "conclusion": "Dashboard tersedia"},
        ],
        "criteria": [
            {
                "id": "result-analysis", "order": 0, "profile_criterion_id": "mandatory-analysis", "official_id": "K1", "title": "Analisis lengkap",
                "category": "mandatory", "status": "MET", "authority": "AUTHORITATIVE_REQUIREMENT", "rubric_refs": [rubric_ref("K1")],
                "evidence_refs": ["ev-notebook"], "manual_owner": {"owner_type": "PARENT", "owner_id": "reviewer", "reviewed_at": T}, "conclusion": "Terpenuhi",
            },
            {
                "id": "result-dashboard", "order": 1, "profile_criterion_id": "mandatory-dashboard", "official_id": "K2", "title": "Dashboard interaktif",
                "category": "mandatory", "status": "MET", "authority": "AUTHORITATIVE_REQUIREMENT", "rubric_refs": [rubric_ref("K2")],
                "evidence_refs": ["ev-source", "ev-browser"], "manual_owner": {"owner_type": "PARENT", "owner_id": "reviewer", "reviewed_at": T}, "conclusion": "Terpenuhi",
            },
            {
                "id": "result-doc", "order": 2, "profile_criterion_id": "optional-documentation", "official_id": "S1", "title": "Dokumentasi",
                "category": "optional", "status": "MET", "authority": "AUTHORITATIVE_REQUIREMENT", "rubric_refs": [rubric_ref("S1")],
                "evidence_refs": ["ev-readme"], "manual_owner": {"owner_type": "PARENT", "owner_id": "reviewer", "reviewed_at": T}, "conclusion": "Terpenuhi", "points_awarded": 1,
            },
            {
                "id": "result-advanced", "order": 3, "profile_criterion_id": "optional-analysis", "official_id": "S2", "title": "Analisis lanjutan",
                "category": "optional", "status": "MET", "authority": "AUTHORITATIVE_REQUIREMENT", "rubric_refs": [rubric_ref("S2")],
                "evidence_refs": ["ev-notebook"], "manual_owner": {"owner_type": "PARENT", "owner_id": "reviewer", "reviewed_at": T}, "conclusion": "Terpenuhi", "points_awarded": 1,
            },
        ],
        "runtime_results": [{
            "id": "runtime-browser", "order": 0, "profile_obligation_id": "dashboard-browser", "status": "PASSED",
            "owner_id": "agent-browser", "target_resource": "browser:9222", "evidence_refs": ["ev-browser"], "conclusion": "Interaksi berhasil",
        }],
        "integrity": {"checks": [
            {"id": "integrity-manual", "order": 0, "kind": "MANUAL_PLAGIARISM", "status": "CLEAR", "evidence_refs": ["ev-integrity"], "conclusion": "Tidak ditemukan kemiripan material"}
        ]},
        "crosschecks": [],
        "artifacts": [{
            "id": "approval-letter", "order": 0, "kind": "APPROVAL_LETTER", "audience": "STUDENT", "template_id": "approval",
            "status": "FINAL", "reference": str(APPROVAL_PATH), "sha256": file_digest(APPROVAL_PATH), "decision_label": "LULUS", "rating_value": 4,
        }],
        "decision": {
            "status": "LULUS", "scoring": {"mode": "optional_count", "eligible": True, "value": 4, "maximum": 4,
            "included_criterion_refs": ["result-doc", "result-advanced"], "calculation": "2 + 2 = 4"},
            "criterion_refs": ["result-analysis", "result-dashboard", "result-doc", "result-advanced"],
            "artifact_refs": ["approval-letter"], "rationale": "Semua kriteria wajib terpenuhi",
        },
        "audit": {
            "workflow_state": "READY", "validator_runs": [{"id": "audit-1", "order": 0, "tool_version": "1.0.0", "profile_sha256": H, "packet_sha256": H, "command": "python3 scripts/audit_review.py --profile profile.json --packet packet.json", "executed_at": T, "result": "READY", "issue_count": 0}],
            "manual_signoff": {"signed": True, "reviewer_id": "reviewer", "signed_at": T}, "updated_at": T,
        },
    }
    return seal_packet(profile, packet)


def dashboard_review_v11():
    profile = dashboard_profile()
    profile["schema_version"] = "1.1.0"
    for criterion in profile["criteria"]:
        criterion["audience_labels"] = {
            "reviewer": {"id": criterion["official_id"], "title": criterion["title"]},
            "student": {"id": f"Student {criterion['official_id']}", "title": criterion["title"]},
        }
        criterion["authority_refs"] = [
            rule_ref(reference["locator"])
            for reference in criterion["authority_refs"]
        ]

    packet = dashboard_packet()
    packet["schema_version"] = "1.1.0"
    packet["profile"]["schema_version"] = "1.1.0"
    definitions = {criterion["id"]: criterion for criterion in profile["criteria"]}
    for result in packet["criteria"]:
        definition = definitions[result["profile_criterion_id"]]
        result["audience_labels"] = copy.deepcopy(definition["audience_labels"])
        result["rubric_refs"] = copy.deepcopy(definition["authority_refs"])
    for item in packet["evidence"]:
        if item["provenance"] == "DERIVED":
            item["source_evidence_id"] = item["derived_from_refs"][0]
    return profile, seal_packet(profile, packet)


def etl_profile():
    profile = dashboard_profile()
    profile.update({
        "profile_id": "etl-project", "profile_version": "3.1.0",
        "course": {"id": "etl", "title": "Pemrosesan Data", "project_title": "ETL Pipeline", "rubric_version": "3.0.0"},
        "artifact_types": [{"id": "source", "order": 0, "title": "Source", "required": True, "accepted_extensions": [".py"]}],
        "criteria": [
            {"id": "mandatory-etl", "order": 0, "official_id": "K1", "title": "ETL berjalan", "category": "mandatory", "authority_refs": [rubric_ref("K1")], "required_evidence_kinds": ["source", "test"], "manual_review_required": True, "runtime_obligation_refs": ["tests"]},
            {"id": "optional-tests", "order": 1, "official_id": "S1", "title": "Kualitas test", "category": "optional", "authority_refs": [rubric_ref("S1")], "required_evidence_kinds": ["test"], "manual_review_required": True, "runtime_obligation_refs": [], "points": 5},
            {"id": "optional-repository", "order": 2, "official_id": "S2", "title": "Repository", "category": "optional", "authority_refs": [rubric_ref("S2")], "required_evidence_kinds": ["repository"], "manual_review_required": True, "runtime_obligation_refs": [], "points": 3},
        ],
        "runtime_obligations": [{"id": "tests", "order": 0, "title": "Test suite", "kind": "test_suite", "required": True, "browser_trace_required": False, "trigger": {"type": "ALWAYS"}}],
        "scoring": {"mode": "average_points", "optional_scoring_requires_mandatory_pass": True, "base_score": 0, "maximum_score": 5, "tiers": [{"id": "good", "order": 0, "label": "Baik", "minimum": 4, "maximum": 5}]},
        "delegation_roles": [{"id": "source-review", "order": 0, "title": "Source review", "capability": "runtime", "eligible_in_full_review": True, "exclusive_resource": "runtime:test-suite"}],
    })
    return profile


def etl_packet():
    profile = etl_profile()
    packet = dashboard_packet()
    packet["review_id"] = "review-etl"
    packet["submission"].update({"id": "submission-etl", "course_id": "etl"})
    packet["profile"].update({"profile_id": "etl-project", "profile_version": "3.1.0", "rubric_version": "3.0.0"})
    packet["capabilities"]["role_assignments"] = [{"id": "role-source", "order": 0, "profile_role_id": "source-review", "status": "DELEGATED", "owner_type": "SUBAGENT", "owner_id": "agent-source", "exclusive_resource": "runtime:test-suite"}]
    packet["evidence"] = [
        evidence("ev-note", 0, "student_note"), evidence("ev-source", 1, "source"), evidence("ev-tests", 2, "test"),
        evidence("ev-repository", 3, "repository"), evidence("ev-auto-raw", 4, "log"),
        evidence("ev-auto-derived", 5, "log", "DERIVED", ["ev-auto-raw"]), evidence("ev-integrity", 6, "source"),
    ]
    packet["submission_artifacts"] = [
        {"id": "submitted-source", "order": 0, "profile_artifact_type_id": "source", "status": "PRESENT", "evidence_refs": ["ev-source"], "conclusion": "Source tersedia"}
    ]
    packet["criteria"] = [
        {"id": "result-etl", "order": 0, "profile_criterion_id": "mandatory-etl", "official_id": "K1", "title": "ETL berjalan", "category": "mandatory", "status": "MET", "authority": "AUTHORITATIVE_REQUIREMENT", "rubric_refs": [rubric_ref("K1")], "evidence_refs": ["ev-source", "ev-tests"], "manual_owner": {"owner_type": "PARENT", "owner_id": "reviewer", "reviewed_at": T}, "conclusion": "Terpenuhi"},
        {"id": "result-tests", "order": 1, "profile_criterion_id": "optional-tests", "official_id": "S1", "title": "Kualitas test", "category": "optional", "status": "MET", "authority": "AUTHORITATIVE_REQUIREMENT", "rubric_refs": [rubric_ref("S1")], "evidence_refs": ["ev-tests"], "manual_owner": {"owner_type": "PARENT", "owner_id": "reviewer", "reviewed_at": T}, "conclusion": "Terpenuhi", "points_awarded": 5},
        {"id": "result-repository", "order": 2, "profile_criterion_id": "optional-repository", "official_id": "S2", "title": "Repository", "category": "optional", "status": "MET", "authority": "AUTHORITATIVE_REQUIREMENT", "rubric_refs": [rubric_ref("S2")], "evidence_refs": ["ev-repository"], "manual_owner": {"owner_type": "PARENT", "owner_id": "reviewer", "reviewed_at": T}, "conclusion": "Terpenuhi", "points_awarded": 3},
    ]
    packet["runtime_results"] = [{"id": "runtime-tests", "order": 0, "profile_obligation_id": "tests", "status": "PASSED", "owner_id": "agent-source", "target_resource": "runtime:test-suite", "evidence_refs": ["ev-tests"], "conclusion": "Test lulus"}]
    packet["decision"]["scoring"] = {"mode": "average_points", "eligible": True, "value": 4, "maximum": 5, "included_criterion_refs": ["result-tests", "result-repository"], "calculation": "(5 + 3) / 2 = 4"}
    packet["decision"]["criterion_refs"] = ["result-etl", "result-tests", "result-repository"]
    return seal_packet(profile, packet)


class AuditorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("audit_review", AUDITOR_PATH)
        cls.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.module)

    def audit(self, profile=None, packet=None):
        return self.module.audit(profile or dashboard_profile(), packet or dashboard_packet())

    def codes(self, result):
        return {issue["code"] for issue in result["issues"]}

    def assert_invalid(self, packet, code, profile=None):
        result = self.audit(profile or dashboard_profile(), packet)
        self.assertEqual("INVALID", result["status"])
        self.assertIn(code, self.codes(result))

    def test_valid_dashboard_optional_count_is_ready(self):
        self.assertEqual({"status": "READY", "exit_code": 0, "issues": []}, self.audit())

    def test_historical_v1_packet_remains_ready(self):
        profile = dashboard_profile()
        packet = dashboard_packet()
        self.assertEqual("1.0.0", profile["schema_version"])
        self.assertEqual("1.0.0", packet["schema_version"])
        self.assertEqual({"status": "READY", "exit_code": 0, "issues": []}, self.audit(profile, packet))

    def test_historical_v1_packet_still_requires_current_auditor_run(self):
        profile = dashboard_profile()
        packet = dashboard_packet()
        packet["audit"]["validator_runs"][-1]["tool_version"] = "1.0.0"
        packet["audit"]["validator_runs"][-1]["packet_sha256"] = packet_subject_digest(packet)
        self.assert_invalid(packet, "VALIDATOR_VERSION_MISMATCH", profile)

    def test_v11_stable_ids_allow_uppercase_badp_internal_ids(self):
        profile, packet = dashboard_review_v11()
        profile["criteria"][0]["id"] = "K0"
        packet["criteria"][0]["profile_criterion_id"] = "K0"
        packet = seal_packet(profile, packet)
        self.assertEqual(
            {"status": "READY", "exit_code": 0, "issues": []},
            self.audit(profile, packet),
        )

    def test_v11_allows_distinct_student_and_reviewer_labels(self):
        profile, packet = dashboard_review_v11()
        self.assertNotEqual(
            profile["criteria"][0]["audience_labels"]["student"]["id"],
            profile["criteria"][0]["audience_labels"]["reviewer"]["id"],
        )
        self.assertEqual({"status": "READY", "exit_code": 0, "issues": []}, self.audit(profile, packet))

    def test_v11_profile_requires_student_audience_label(self):
        profile, packet = dashboard_review_v11()
        del profile["criteria"][0]["audience_labels"]["student"]
        packet = seal_packet(profile, packet)
        self.assert_invalid(packet, "SCHEMA_VALIDATION", profile)

    def test_v11_profile_requires_reviewer_audience_label(self):
        profile, packet = dashboard_review_v11()
        del profile["criteria"][0]["audience_labels"]["reviewer"]
        packet = seal_packet(profile, packet)
        self.assert_invalid(packet, "SCHEMA_VALIDATION", profile)

    def test_v11_audience_label_rejects_additional_fields(self):
        profile, packet = dashboard_review_v11()
        profile["criteria"][0]["audience_labels"]["reviewer"]["number"] = 1
        packet = seal_packet(profile, packet)
        self.assert_invalid(packet, "SCHEMA_VALIDATION", profile)

    def test_v11_profile_official_id_must_match_reviewer_label(self):
        profile, packet = dashboard_review_v11()
        profile["criteria"][0]["official_id"] = "Reviewer K99"
        packet = seal_packet(profile, packet)
        self.assert_invalid(packet, "CRITERION_AUDIENCE_LABEL_MISMATCH", profile)

    def test_v11_packet_labels_must_match_locked_profile(self):
        profile, packet = dashboard_review_v11()
        packet["criteria"][0]["audience_labels"]["student"]["id"] = "Wrong student label"
        packet = seal_packet(profile, packet)
        self.assert_invalid(packet, "CRITERION_AUDIENCE_LABEL_MISMATCH", profile)

    def test_v11_packet_requires_audience_labels(self):
        profile, packet = dashboard_review_v11()
        del packet["criteria"][0]["audience_labels"]
        packet = seal_packet(profile, packet)
        self.assert_invalid(packet, "SCHEMA_VALIDATION", profile)

    def test_v11_rule_role_is_closed_enum(self):
        profile, packet = dashboard_review_v11()
        profile["criteria"][0]["authority_refs"][0]["rule_role"] = "internal_gate"
        packet = seal_packet(profile, packet)
        self.assert_invalid(packet, "SCHEMA_VALIDATION", profile)

    def test_v11_rule_reference_requires_authority_class_and_outcome_effect(self):
        profile, packet = dashboard_review_v11()
        del profile["criteria"][0]["authority_refs"][0]["authority_class"]
        del profile["criteria"][1]["authority_refs"][0]["outcome_effect"]
        packet = seal_packet(profile, packet)
        self.assert_invalid(packet, "SCHEMA_VALIDATION", profile)

    def test_v11_packet_rule_reference_requires_rule_metadata(self):
        profile, packet = dashboard_review_v11()
        del packet["criteria"][0]["rubric_refs"][0]["authority_class"]
        packet = seal_packet(profile, packet)
        self.assert_invalid(packet, "SCHEMA_VALIDATION", profile)

    def test_v11_criterion_outcome_cannot_rely_only_on_non_decisive_rule(self):
        for role in ("diagnostic_case", "development_note"):
            with self.subTest(role=role):
                profile, packet = dashboard_review_v11()
                advisory = rule_ref("K1 example", role, False)
                profile["criteria"][0]["authority_refs"] = [advisory]
                packet["criteria"][0]["rubric_refs"] = [copy.deepcopy(advisory)]
                packet = seal_packet(profile, packet)
                self.assert_invalid(packet, "CRITERION_OUTCOME_BASIS_INVALID", profile)

    def test_v11_criterion_outcome_requires_explicit_outcome_effect(self):
        profile, packet = dashboard_review_v11()
        profile["criteria"][0]["authority_refs"][0]["outcome_effect"] = False
        packet["criteria"][0]["rubric_refs"] = copy.deepcopy(
            profile["criteria"][0]["authority_refs"]
        )
        packet = seal_packet(profile, packet)
        self.assert_invalid(packet, "CRITERION_OUTCOME_BASIS_INVALID", profile)

    def test_v11_rule_metadata_must_be_intrinsically_consistent(self):
        cases = (
            ("diagnostic_case", True, "GUIDANCE"),
            ("development_note", True, "REVIEWER_INFERENCE"),
            ("official_gate", False, "AUTHORITATIVE_REQUIREMENT"),
            ("common_case", False, "AUTHORITATIVE_EXCEPTION"),
            ("official_gate", True, "REVIEWER_INFERENCE"),
            ("common_case", True, "GUIDANCE"),
            ("diagnostic_case", False, "AUTHORITATIVE_REQUIREMENT"),
        )
        for role, outcome_effect, authority_class in cases:
            with self.subTest(
                role=role,
                outcome_effect=outcome_effect,
                authority_class=authority_class,
            ):
                profile, packet = dashboard_review_v11()
                reference = rule_ref(
                    "K1 rule",
                    role=role,
                    outcome_effect=outcome_effect,
                    authority_class=authority_class,
                )
                profile["criteria"][0]["authority_refs"] = [reference]
                packet["criteria"][0]["rubric_refs"] = [copy.deepcopy(reference)]
                packet = seal_packet(profile, packet)
                self.assert_invalid(packet, "RULE_METADATA_INCONSISTENT", profile)

    def test_profile_and_packet_schema_versions_must_match(self):
        profile, packet = dashboard_review_v11()
        packet["schema_version"] = "1.0.0"
        packet = seal_packet(profile, packet)
        self.assert_invalid(packet, "SCHEMA_VERSION_MISMATCH", profile)

        profile, packet = dashboard_review_v11()
        profile["schema_version"] = "1.0.0"
        packet["profile"]["schema_version"] = "1.0.0"
        packet = seal_packet(profile, packet)
        self.assert_invalid(packet, "SCHEMA_VERSION_MISMATCH", profile)

    def test_v11_derived_evidence_requires_source_evidence_id(self):
        profile, packet = dashboard_review_v11()
        del packet["evidence"][4]["source_evidence_id"]
        packet = seal_packet(profile, packet)
        self.assert_invalid(packet, "SCHEMA_VALIDATION", profile)

    def test_v11_source_evidence_id_must_point_to_available_raw_evidence(self):
        profile, packet = dashboard_review_v11()
        packet["evidence"][4]["source_evidence_id"] = "ev-auto-derived"
        packet = seal_packet(profile, packet)
        self.assert_invalid(packet, "INVALID_SOURCE_EVIDENCE_ID", profile)

    def test_valid_etl_average_points_is_ready(self):
        self.assertEqual({"status": "READY", "exit_code": 0, "issues": []}, self.audit(etl_profile(), etl_packet()))

    def test_duplicate_ids_are_rejected(self):
        packet = dashboard_packet()
        packet["evidence"][1]["id"] = "ev-note"
        self.assert_invalid(packet, "DUPLICATE_ID")

    def test_duplicate_orders_are_rejected(self):
        packet = dashboard_packet()
        packet["criteria"][1]["order"] = 0
        self.assert_invalid(packet, "DUPLICATE_ORDER")

    def test_broken_evidence_reference_is_rejected(self):
        packet = dashboard_packet()
        packet["criteria"][0]["evidence_refs"] = ["ev-missing"]
        self.assert_invalid(packet, "BROKEN_EVIDENCE_REF")

    def test_broken_profile_reference_is_rejected(self):
        packet = dashboard_packet()
        packet["criteria"][0]["profile_criterion_id"] = "missing"
        self.assert_invalid(packet, "BROKEN_PROFILE_REF")

    def test_autograder_only_mandatory_decision_is_rejected(self):
        packet = dashboard_packet()
        packet["criteria"][0]["authority"] = "AUTOMATION_SIGNAL"
        packet["criteria"][0]["evidence_refs"] = ["ev-auto-derived"]
        self.assert_invalid(packet, "AUTOGRADER_ONLY_DECISION")

    def test_required_browser_obligation_needs_result(self):
        packet = dashboard_packet()
        packet["runtime_results"] = []
        self.assert_invalid(packet, "MISSING_RUNTIME_RESULT")

    def test_required_browser_obligation_needs_browser_trace(self):
        packet = dashboard_packet()
        del packet["evidence"][4]["browser_trace"]
        self.assert_invalid(packet, "BROWSER_TRACE_REQUIRED")

    def test_browser_trace_fields_cannot_be_empty(self):
        packet = dashboard_packet()
        packet["evidence"][4]["browser_trace"]["after"] = ""
        self.assert_invalid(packet, "SCHEMA_VALIDATION")

    def test_optional_scoring_is_ineligible_when_mandatory_failed(self):
        packet = dashboard_packet()
        packet["criteria"][0]["status"] = "NOT_MET"
        packet["criteria"][0]["crosscheck_refs"] = ["cross-analysis"]
        packet["crosschecks"] = [{"id": "cross-analysis", "order": 0, "subject_type": "NEGATIVE_FINDING", "subject_ref": "result-analysis", "status": "CONFIRMED", "authority_refs": [rubric_ref("K1")], "evidence_refs": ["ev-notebook"], "owner_id": "reviewer", "conclusion": "Terkonfirmasi"}]
        self.assert_invalid(packet, "SCORING_NOT_ELIGIBLE")

    def test_optional_count_calculation_must_match_profile(self):
        packet = dashboard_packet()
        packet["decision"]["scoring"]["value"] = 3
        self.assert_invalid(packet, "SCORING_VALUE_MISMATCH")

    def test_average_points_can_score_mandatory_criteria(self):
        profile = etl_profile()
        packet = etl_packet()

        for definition in profile["criteria"]:
            definition["category"] = "mandatory"
            definition["points"] = 4
        for result in packet["criteria"]:
            result["category"] = "mandatory"
            result["points_awarded"] = 4

        packet["decision"]["scoring"] = {
            "mode": "average_points",
            "eligible": True,
            "value": 4,
            "maximum": 5,
            "included_criterion_refs": [
                "result-etl",
                "result-tests",
                "result-repository",
            ],
            "calculation": "(4 + 4 + 4) / 3 = 4",
        }
        packet = seal_packet(profile, packet)

        self.assertEqual(
            {"status": "READY", "exit_code": 0, "issues": []},
            self.audit(profile, packet),
        )

    def test_average_points_calculation_must_match_profile(self):
        packet = etl_packet()
        packet["decision"]["scoring"]["value"] = 5
        self.assert_invalid(packet, "SCORING_VALUE_MISMATCH", etl_profile())

    def test_included_scoring_refs_must_be_optional(self):
        packet = dashboard_packet()
        packet["decision"]["scoring"]["included_criterion_refs"] = ["result-analysis", "result-doc"]
        self.assert_invalid(packet, "INVALID_SCORING_REF")

    def test_full_review_requires_assignment_for_eligible_role(self):
        packet = dashboard_packet()
        packet["capabilities"]["role_assignments"] = packet["capabilities"]["role_assignments"][:1]
        self.assert_invalid(packet, "MISSING_ROLE_ASSIGNMENT")

    def test_parent_owned_role_requires_reason(self):
        packet = dashboard_packet()
        packet["capabilities"]["role_assignments"][0].update({"status": "PARENT_OWNED", "owner_type": "PARENT", "owner_id": "reviewer"})
        self.assert_invalid(packet, "PARENT_OWNED_REASON_REQUIRED")

    def test_available_multi_agent_requires_eligible_role_delegation(self):
        packet = dashboard_packet()
        packet["capabilities"]["role_assignments"][0].update({
            "status": "PARENT_OWNED", "owner_type": "PARENT", "owner_id": "reviewer",
            "capability_gate_reason": "Reviewer prefers direct inspection",
        })
        self.assert_invalid(packet, "DELEGATION_REQUIRED")

    def test_parent_owned_role_accepts_structured_exclusive_resource_constraint(self):
        profile = dashboard_profile()
        packet = dashboard_packet()
        packet["capabilities"]["role_assignments"][1].update({
            "status": "PARENT_OWNED", "owner_type": "PARENT", "owner_id": "reviewer",
            "capability_gate_reason": "Existing browser session cannot be isolated",
            "non_isolation_reason": {
                "kind": "EXCLUSIVE_RESOURCE",
                "detail": "Existing browser session cannot be isolated",
                "exclusive_resource": "browser:9222",
            },
        })
        packet["runtime_results"][0]["owner_id"] = "reviewer"
        packet["evidence"][4]["browser_trace"]["owner_id"] = "reviewer"
        seal_packet(profile, packet)
        self.assertEqual("READY", self.audit(profile, packet)["status"])

    def test_parent_owned_capability_constraint_rejects_available_capability(self):
        profile, packet = dashboard_profile(), dashboard_packet()
        packet["capabilities"]["role_assignments"][1].update({
            "status": "PARENT_OWNED", "owner_type": "PARENT", "owner_id": "reviewer",
            "capability_gate_reason": "Browser capability is unavailable",
            "non_isolation_reason": {
                "kind": "CAPABILITY_CONSTRAINT",
                "detail": "Browser capability is unavailable",
                "capability": "browser",
            },
        })
        packet["runtime_results"][0]["owner_id"] = "reviewer"
        packet["evidence"][4]["browser_trace"]["owner_id"] = "reviewer"
        seal_packet(profile, packet)
        self.assert_invalid(packet, "DELEGATION_REQUIRED", profile)
    def test_parent_owned_preference_reason_kind_is_rejected(self):
        packet = dashboard_packet()
        packet["capabilities"]["role_assignments"][0].update({
            "status": "PARENT_OWNED", "owner_type": "PARENT", "owner_id": "reviewer",
            "capability_gate_reason": "Reviewer preference",
            "non_isolation_reason": {"kind": "PREFERENCE", "detail": "Reviewer preference"},
        })
        self.assert_invalid(packet, "SCHEMA_VALIDATION")

    def test_delegated_role_requires_subagent_owner(self):
        packet = dashboard_packet()
        packet["capabilities"]["role_assignments"][0]["owner_type"] = "PARENT"
        self.assert_invalid(packet, "INVALID_ROLE_OWNER")

    def test_exclusive_resource_cannot_have_multiple_owners(self):
        profile = dashboard_profile()
        profile["delegation_roles"].append({"id": "browser-second", "order": 2, "title": "Browser kedua", "capability": "browser", "eligible_in_full_review": True, "exclusive_resource": "browser:9222"})
        packet = dashboard_packet()
        packet["capabilities"]["role_assignments"].append({"id": "role-browser-second", "order": 2, "profile_role_id": "browser-second", "status": "DELEGATED", "owner_type": "SUBAGENT", "owner_id": "another-agent", "exclusive_resource": "browser:9222"})
        self.assert_invalid(packet, "EXCLUSIVE_RESOURCE_CONFLICT", profile)

    def test_raw_evidence_cannot_derive_from_other_evidence(self):
        packet = dashboard_packet()
        packet["evidence"][0]["derived_from_refs"] = ["ev-notebook"]
        self.assert_invalid(packet, "RAW_EVIDENCE_HAS_PROVENANCE")

    def test_derived_evidence_must_point_directly_to_raw_evidence(self):
        packet = dashboard_packet()
        packet["evidence"].append(evidence("ev-derived-two", 8, "log", "DERIVED", ["ev-auto-derived"]))
        self.assert_invalid(packet, "INVALID_DERIVED_PROVENANCE")

    def test_automation_derived_refs_must_be_derived(self):
        packet = dashboard_packet()
        packet["automation_runs"][0]["derived_evidence_refs"] = ["ev-auto-raw"]
        self.assert_invalid(packet, "INVALID_AUTOMATION_PROVENANCE")

    def test_negative_criterion_requires_crosscheck(self):
        packet = dashboard_packet()
        packet["criteria"][2]["status"] = "NOT_MET"
        packet["criteria"][2]["points_awarded"] = 0
        packet["decision"]["scoring"].update({"value": 3, "included_criterion_refs": ["result-advanced"], "calculation": "2 + 1 = 3"})
        self.assert_invalid(packet, "CROSSCHECK_REQUIRED")

    def test_crosscheck_must_reference_subject(self):
        packet = dashboard_packet()
        packet["criteria"][2]["status"] = "NOT_MET"
        packet["criteria"][2]["points_awarded"] = 0
        packet["criteria"][2]["crosscheck_refs"] = ["cross-doc"]
        packet["crosschecks"] = [{"id": "cross-doc", "order": 0, "subject_type": "NEGATIVE_FINDING", "subject_ref": "missing", "status": "CONFIRMED", "authority_refs": [rubric_ref("S1")], "evidence_refs": ["ev-readme"], "owner_id": "reviewer", "conclusion": "Terkonfirmasi"}]
        self.assert_invalid(packet, "BROKEN_SUBJECT_REF")

    def test_confirmed_negative_cannot_use_overturned_crosscheck(self):
        packet = dashboard_packet()
        packet["criteria"][2]["status"] = "NOT_MET"
        packet["criteria"][2]["points_awarded"] = 0
        packet["criteria"][2]["crosscheck_refs"] = ["cross-doc"]
        packet["crosschecks"] = [{"id": "cross-doc", "order": 0, "subject_type": "NEGATIVE_FINDING", "subject_ref": "result-doc", "status": "OVERTURNED", "authority_refs": [rubric_ref("S1")], "evidence_refs": ["ev-readme"], "owner_id": "reviewer", "conclusion": "Temuan dibatalkan"}]
        packet["decision"]["scoring"].update({"value": 3, "included_criterion_refs": ["result-advanced"], "calculation": "2 + 1 = 3"})
        self.assert_invalid(packet, "CROSSCHECK_STATUS_MISMATCH")

    def test_failed_runtime_requires_crosscheck(self):
        packet = dashboard_packet()
        packet["runtime_results"][0]["status"] = "FAILED"
        self.assert_invalid(packet, "CROSSCHECK_REQUIRED")

    def test_revision_accepts_crosschecked_failed_required_runtime(self):
        profile = dashboard_profile()
        packet = dashboard_packet()
        packet["decision"]["status"] = "REVISI"
        packet["runtime_results"][0]["status"] = "FAILED"
        packet["crosschecks"].append({
            "id": "cross-runtime",
            "order": len(packet["crosschecks"]),
            "subject_type": "NEGATIVE_FINDING",
            "subject_ref": packet["runtime_results"][0]["id"],
            "status": "CONFIRMED",
            "authority_refs": [rubric_ref("K1")],
            "evidence_refs": list(packet["runtime_results"][0]["evidence_refs"]),
            "owner_id": "reviewer",
            "conclusion": "Runtime failure confirmed for revision.",
        })

        issues = []
        self.module._runtime_semantics(profile, packet, issues)
        codes = {issue["code"] for issue in issues}

        self.assertNotIn("CROSSCHECK_REQUIRED", codes)
        self.assertNotIn("REQUIRED_RUNTIME_NOT_PASSED", codes)

    def test_flagged_integrity_check_requires_crosscheck(self):
        packet = dashboard_packet()
        packet["integrity"]["checks"][0]["status"] = "FLAGGED"
        self.assert_invalid(packet, "CROSSCHECK_REQUIRED")

    def test_skipped_criterion_requires_authorized_skip_rule(self):
        profile = dashboard_profile()
        profile["skip_rules"] = [{"id": "skip-optional", "order": 0, "applies_to": "optional", "condition": "Not submitted", "reason": "Optional"}]
        packet = dashboard_packet()
        packet["criteria"][2]["status"] = "SKIPPED"
        packet["criteria"][2]["points_awarded"] = 0
        packet["decision"]["scoring"].update({"value": 3, "included_criterion_refs": ["result-advanced"], "calculation": "2 + 1 = 3"})
        self.assert_invalid(packet, "SKIP_RULE_REQUIRED", profile)

    def test_skip_rule_must_apply_to_criterion_category(self):
        profile = dashboard_profile()
        profile["skip_rules"] = [{"id": "skip-mandatory", "order": 0, "applies_to": "mandatory", "condition": "Blocked", "reason": "Explicit rule"}]
        packet = dashboard_packet()
        packet["criteria"][2].update({"status": "SKIPPED", "points_awarded": 0, "skip_rule_ref": "skip-mandatory"})
        packet["decision"]["scoring"].update({"value": 3, "included_criterion_refs": ["result-advanced"], "calculation": "2 + 1 = 3"})
        self.assert_invalid(packet, "SKIP_RULE_MISMATCH", profile)

    def test_skip_rule_requires_authoritative_source_and_locator(self):
        profile = dashboard_profile()
        profile["skip_rules"] = [{
            "id": "skip-optional", "order": 0, "applies_to": "optional",
            "condition": "Optional review omitted", "reason": "Official exception",
        }]
        packet = seal_packet(profile, dashboard_packet())
        self.assert_invalid(packet, "SKIP_RULE_AUTHORITY_REQUIRED", profile)

    def test_skip_rule_cannot_cite_automation_authority(self):
        profile = dashboard_profile()
        profile["skip_rules"] = [{
            "id": "skip-optional", "order": 0, "applies_to": "optional",
            "condition": "Optional review omitted", "reason": "Tool signal",
            "authority_ref": {"authority_id": "autograder", "locator": "rule 4"},
        }]
        packet = seal_packet(profile, dashboard_packet())
        self.assert_invalid(packet, "SKIP_RULE_AUTHORITY_INVALID", profile)

    def test_student_letter_rejects_internal_path_content(self):
        with tempfile.TemporaryDirectory() as directory:
            letter = Path(directory) / "approval.md"
            letter.write_text("Bukti tersedia di /home/raya/private/review.log", encoding="utf-8")
            packet = dashboard_packet()
            packet["artifacts"][0]["reference"] = str(letter)
            self.assert_invalid(packet, "UNSAFE_STUDENT_CONTENT")

    def test_student_letter_rejects_mnt_absolute_posix_path(self):
        with tempfile.TemporaryDirectory() as directory:
            letter = Path(directory) / "approval.md"
            letter.write_text("Status: LULUS\nRating: 4\nBukti: /mnt/reviewer/submission/output.log\n", encoding="utf-8")
            packet = dashboard_packet()
            packet["artifacts"][0].update({"reference": str(letter), "sha256": file_digest(letter)})
            self.assert_invalid(packet, "UNSAFE_STUDENT_CONTENT")

    def test_student_letter_rejects_placeholder_content(self):
        with tempfile.TemporaryDirectory() as directory:
            letter = Path(directory) / "approval.md"
            letter.write_text("Selamat, {{student_name}}. TODO: lengkapi.", encoding="utf-8")
            packet = dashboard_packet()
            packet["artifacts"][0]["reference"] = str(letter)
            self.assert_invalid(packet, "PLACEHOLDER_IN_STUDENT_CONTENT")

    def test_lulus_requires_final_approval_letter(self):
        packet = dashboard_packet()
        packet["artifacts"][0]["status"] = "DRAFT"
        self.assert_invalid(packet, "DECISION_ARTIFACT_MISMATCH")

    def test_revisi_requires_matching_rejection_letter(self):
        packet = dashboard_packet()
        packet["decision"]["status"] = "REVISI"
        self.assert_invalid(packet, "DECISION_ARTIFACT_MISMATCH")

    def test_revisi_accepts_partial_mandatory_criterion_as_non_pass(self):
        profile = dashboard_profile()
        packet = dashboard_packet()
        packet["criteria"][0].update({"status": "PARTIAL", "crosscheck_refs": ["cross-analysis"]})
        packet["crosschecks"] = [{
            "id": "cross-analysis", "order": 0, "subject_type": "NEGATIVE_FINDING",
            "subject_ref": "result-analysis", "status": "CONFIRMED",
            "authority_refs": [rubric_ref("K1")], "evidence_refs": ["ev-notebook"],
            "owner_id": "reviewer", "conclusion": "Only part of K1 is met",
        }]
        packet["artifacts"][0].update({
            "kind": "REJECTION_LETTER", "template_id": "rejection",
            "reference": str(REJECTION_PATH), "sha256": file_digest(REJECTION_PATH),
            "decision_label": "REVISI",
        })
        packet["artifacts"][0].pop("rating_value")
        packet["decision"].update({
            "status": "REVISI", "scoring": {
                "mode": "optional_count", "eligible": False, "value": None, "maximum": None,
                "included_criterion_refs": [], "calculation": "Mandatory criterion is partial",
            },
        })
        seal_packet(profile, packet)
        self.assertEqual("READY", self.audit(profile, packet)["status"])

    def test_lulus_cannot_have_failed_mandatory_criterion(self):
        packet = dashboard_packet()
        packet["criteria"][0]["status"] = "NOT_MET"
        packet["criteria"][0]["crosscheck_refs"] = ["cross-analysis"]
        packet["crosschecks"] = [{"id": "cross-analysis", "order": 0, "subject_type": "NEGATIVE_FINDING", "subject_ref": "result-analysis", "status": "CONFIRMED", "authority_refs": [rubric_ref("K1")], "evidence_refs": ["ev-notebook"], "owner_id": "reviewer", "conclusion": "Terkonfirmasi"}]
        self.assert_invalid(packet, "DECISION_CRITERIA_MISMATCH")

    def test_final_decision_must_reference_every_criterion_result(self):
        packet = dashboard_packet()
        packet["decision"]["criterion_refs"].remove("result-dashboard")
        self.assert_invalid(packet, "DECISION_CRITERION_REFS_MISMATCH")

    def test_final_full_review_requires_parent_manual_owner(self):
        packet = dashboard_packet()
        packet["criteria"][0]["manual_owner"].update({"owner_type": "SUBAGENT", "owner_id": "agent-files"})
        self.assert_invalid(packet, "MANUAL_OWNER_REQUIRED")

    def test_ready_decision_requires_manual_signoff(self):
        packet = dashboard_packet()
        packet["audit"]["manual_signoff"] = {"signed": False, "reviewer_id": "reviewer"}
        self.assert_invalid(packet, "MANUAL_SIGNOFF_REQUIRED")

    def test_ready_decision_requires_ready_workflow(self):
        packet = dashboard_packet()
        packet["audit"]["workflow_state"] = "CONSISTENCY_CHECK"
        self.assert_invalid(packet, "WORKFLOW_NOT_READY")

    def test_pending_decision_cannot_be_reported_ready(self):
        packet = dashboard_packet()
        packet["decision"]["status"] = "PENDING"
        self.assert_invalid(packet, "FINAL_DECISION_REQUIRED")

    def test_mandatory_not_applicable_requires_authorized_rule(self):
        packet = dashboard_packet()
        packet["criteria"][0].update({"status": "NOT_APPLICABLE", "authority": "AUTHORITATIVE_EXCEPTION"})
        self.assert_invalid(packet, "SKIP_RULE_REQUIRED")

    def test_mandatory_not_applicable_requires_evidence(self):
        profile = dashboard_profile()
        profile["skip_rules"] = [{"id": "skip-analysis", "order": 0, "applies_to": "mandatory", "condition": "Tidak berlaku", "reason": "Pengecualian resmi"}]
        packet = dashboard_packet()
        packet["criteria"][0].update({"status": "NOT_APPLICABLE", "authority": "AUTHORITATIVE_EXCEPTION", "skip_rule_ref": "skip-analysis", "evidence_refs": []})
        self.assert_invalid(packet, "SKIP_EVIDENCE_REQUIRED", profile)

    def test_mandatory_skipped_requires_evidence(self):
        profile = dashboard_profile()
        profile["skip_rules"] = [{"id": "skip-analysis", "order": 0, "applies_to": "mandatory", "condition": "Evaluasi dihentikan", "reason": "Aturan resmi"}]
        packet = dashboard_packet()
        packet["criteria"][0].update({"status": "SKIPPED", "authority": "AUTHORITATIVE_EXCEPTION", "skip_rule_ref": "skip-analysis", "evidence_refs": []})
        self.assert_invalid(packet, "SKIP_EVIDENCE_REQUIRED", profile)

    def test_required_submission_artifact_must_be_assessed(self):
        packet = dashboard_packet()
        packet["submission_artifacts"] = packet["submission_artifacts"][:1]
        self.assert_invalid(packet, "REQUIRED_ARTIFACT_UNASSESSED")

    def test_lulus_requires_required_submission_artifact_present(self):
        packet = dashboard_packet()
        packet["submission_artifacts"][1]["status"] = "MISSING"
        self.assert_invalid(packet, "REQUIRED_ARTIFACT_NOT_PRESENT")

    def test_missing_required_submission_artifact_requires_crosscheck(self):
        packet = dashboard_packet()
        packet["submission_artifacts"][1]["status"] = "MISSING"
        self.assert_invalid(packet, "ARTIFACT_CROSSCHECK_REQUIRED")

    def test_eligible_role_cannot_be_marked_not_eligible(self):
        packet = dashboard_packet()
        packet["capabilities"]["role_assignments"][0].update({"status": "NOT_ELIGIBLE", "owner_type": "NONE", "owner_id": "none"})
        self.assert_invalid(packet, "ROLE_ELIGIBILITY_MISMATCH")

    def test_blocked_role_requires_capability_gate_reason(self):
        packet = dashboard_packet()
        packet["capabilities"]["role_assignments"][0].update({"status": "BLOCKED", "owner_type": "NONE", "owner_id": "none"})
        self.assert_invalid(packet, "ROLE_BLOCK_REASON_REQUIRED")

    def test_blocked_role_returns_exit_two_for_blocked_packet(self):
        packet = dashboard_packet()
        packet["capabilities"]["role_assignments"][0].update({"status": "BLOCKED", "owner_type": "NONE", "owner_id": "none", "capability_gate_reason": "Filesystem terisolasi tidak tersedia"})
        packet["decision"].update({"status": "BLOCKED", "artifact_refs": ["approval-letter"], "scoring": {"mode": "optional_count", "eligible": False, "value": None, "maximum": None, "included_criterion_refs": [], "calculation": "Blocked"}})
        packet["artifacts"][0].update({"kind": "REVIEWER_NOTE", "audience": "INTERNAL", "status": "BLOCKED", "decision_label": "BLOCKED"})
        packet["artifacts"][0].pop("template_id")
        packet["artifacts"][0].pop("rating_value")
        packet["audit"].update({"workflow_state": "BLOCKED", "blocked_reason": "Filesystem terisolasi tidak tersedia", "manual_signoff": {"signed": False, "reviewer_id": "reviewer"}})
        result = self.audit(packet=packet)
        self.assertEqual("BLOCKED", result["status"])
        self.assertEqual(2, result["exit_code"])

    def test_relative_student_artifact_is_unreadable(self):
        packet = dashboard_packet()
        packet["artifacts"][0]["reference"] = "approval.md"
        self.assert_invalid(packet, "STUDENT_ARTIFACT_UNREADABLE")

    def test_missing_absolute_student_artifact_is_unreadable(self):
        packet = dashboard_packet()
        packet["artifacts"][0]["reference"] = "/tmp/dicoding-review-auditor-no-such-letter.md"
        self.assert_invalid(packet, "STUDENT_ARTIFACT_UNREADABLE")

    def test_student_artifact_rejects_unredacted_secret(self):
        with tempfile.TemporaryDirectory() as directory:
            letter = Path(directory) / "approval.md"
            letter.write_text("Status: LULUS\nRating: 4\napi_key = sk-testsecret1234567890\n", encoding="utf-8")
            packet = dashboard_packet()
            packet["artifacts"][0].update({"reference": str(letter), "sha256": file_digest(letter)})
            self.assert_invalid(packet, "SECRET_IN_STUDENT_CONTENT")

    def test_student_artifact_content_must_match_decision(self):
        with tempfile.TemporaryDirectory() as directory:
            letter = Path(directory) / "approval.md"
            letter.write_text("Status: REVISI\nRating: 4\n", encoding="utf-8")
            packet = dashboard_packet()
            packet["artifacts"][0].update({"reference": str(letter), "sha256": file_digest(letter)})
            self.assert_invalid(packet, "CONTENT_DECISION_MISMATCH")

    def test_student_artifact_content_must_match_rating(self):
        with tempfile.TemporaryDirectory() as directory:
            letter = Path(directory) / "approval.md"
            letter.write_text("Status: LULUS\nRating: 3\n", encoding="utf-8")
            packet = dashboard_packet()
            packet["artifacts"][0].update({"reference": str(letter), "sha256": file_digest(letter)})
            self.assert_invalid(packet, "CONTENT_RATING_MISMATCH")

    def test_student_artifact_rating_field_must_match_packet(self):
        packet = dashboard_packet()
        packet["artifacts"][0]["rating_value"] = 3
        self.assert_invalid(packet, "ARTIFACT_RATING_MISMATCH")

    def test_student_artifact_digest_must_match_file(self):
        packet = dashboard_packet()
        packet["artifacts"][0]["sha256"] = "b" * 64
        self.assert_invalid(packet, "REFERENCE_DIGEST_MISMATCH")

    def test_selected_template_decision_must_match_packet(self):
        packet = dashboard_packet()
        packet["artifacts"][0]["template_id"] = "rejection"
        self.assert_invalid(packet, "TEMPLATE_DECISION_MISMATCH")

    def test_selected_student_template_cannot_be_internal(self):
        profile = dashboard_profile()
        profile["feedback_templates"][0]["audience"] = "internal"
        packet = seal_packet(profile, dashboard_packet())
        self.assert_invalid(packet, "TEMPLATE_AUDIENCE_MISMATCH", profile)

    def test_internal_screenshot_does_not_require_template(self):
        profile = dashboard_profile()
        packet = dashboard_packet()
        packet["artifacts"].append({"id": "browser-shot", "order": 1, "kind": "SCREENSHOT", "audience": "INTERNAL", "status": "FINAL", "reference": "evidence://shot", "sha256": H})
        seal_packet(profile, packet)
        self.assertEqual("READY", self.audit(profile, packet)["status"])

    def test_profile_requires_official_rubric_and_class_skill(self):
        profile = dashboard_profile()
        profile["authority_sources"] = [profile["authority_sources"][-1]]
        result = self.audit(profile, dashboard_packet())
        self.assertEqual("INVALID", result["status"])
        self.assertIn("OFFICIAL_AUTHORITY_REQUIRED", self.codes(result))

    def test_profile_requires_complete_authority_chain(self):
        profile = dashboard_profile()
        profile["authority_sources"] = [
            source for source in profile["authority_sources"]
            if source["kind"] not in {"official_common_case", "feedback_standard"}
        ]
        packet = seal_packet(profile, dashboard_packet())
        self.assert_invalid(packet, "AUTHORITY_CHAIN_INCOMPLETE", profile)

    def test_profile_rejects_authority_chain_out_of_order(self):
        profile = dashboard_profile()
        profile["authority_sources"][1]["order"] = 3
        profile["authority_sources"][3]["order"] = 1
        packet = seal_packet(profile, dashboard_packet())
        self.assert_invalid(packet, "AUTHORITY_CHAIN_ORDER", profile)

    def test_authority_category_can_be_explicitly_not_applicable_with_official_locator(self):
        profile = dashboard_profile()
        profile["authority_sources"][1].update({
            "applicability": "NOT_APPLICABLE",
            "not_applicable_authority_ref": {"authority_id": "rubric", "locator": "Exceptions section, paragraph 2"},
        })
        packet = seal_packet(profile, dashboard_packet())
        self.assertEqual("READY", self.audit(profile, packet)["status"])

    def test_not_applicable_authority_category_requires_prior_official_source(self):
        profile = dashboard_profile()
        profile["authority_sources"][3].update({
            "applicability": "NOT_APPLICABLE",
            "not_applicable_authority_ref": {"authority_id": "class-skill", "locator": "Feedback section"},
        })
        packet = seal_packet(profile, dashboard_packet())
        self.assert_invalid(packet, "AUTHORITY_NOT_APPLICABLE_UNAUTHORIZED", profile)

    def test_profile_lock_uses_actual_canonical_profile_digest(self):
        packet = dashboard_packet()
        packet["profile"]["sha256"] = H
        self.assert_invalid(packet, "PROFILE_SHA_MISMATCH")

    def test_failed_automation_requires_crosscheck_before_finalization(self):
        packet = dashboard_packet()
        packet["automation_runs"][0].update({"status": "FAILED", "exit_code": 1})
        self.assert_invalid(packet, "AUTOMATION_CROSSCHECK_REQUIRED")

    def test_failed_automation_can_finalize_after_resolved_crosscheck(self):
        profile = dashboard_profile()
        packet = dashboard_packet()
        packet["automation_runs"][0].update({"status": "FAILED", "exit_code": 1})
        packet["crosschecks"] = [{"id": "cross-automation", "order": 0, "subject_type": "AMBIGUOUS_FINDING", "subject_ref": "run-1", "status": "OVERTURNED", "authority_refs": [rubric_ref("automation evidence")], "evidence_refs": ["ev-auto-raw"], "owner_id": "reviewer", "conclusion": "Automation tidak menentukan keputusan"}]
        seal_packet(profile, packet)
        self.assertEqual("READY", self.audit(profile, packet)["status"])

    def test_automation_raw_link_must_match_raw_output(self):
        packet = dashboard_packet()
        packet["automation_runs"][0]["raw_evidence_ref"] = "ev-auto-derived"
        self.assert_invalid(packet, "AUTOMATION_RAW_LINK_MISMATCH")

    def test_automation_tool_version_is_required(self):
        packet = dashboard_packet()
        del packet["automation_runs"][0]["tool_version"]
        self.assert_invalid(packet, "SCHEMA_VALIDATION")

    def test_error_automation_with_null_exit_and_placeholder_command_needs_crosscheck(self):
        packet = dashboard_packet()
        packet["automation_runs"][0].update({"status": "ERROR", "exit_code": None, "command": "UNKNOWN"})
        self.assert_invalid(packet, "AUTOMATION_CROSSCHECK_REQUIRED")

    def test_history_anomaly_requires_crosscheck(self):
        packet = dashboard_packet()
        packet["student_context"]["comparison"].update({"status": "ANOMALY", "summary": "Klaim tidak sesuai histori"})
        self.assert_invalid(packet, "HISTORY_CROSSCHECK_REQUIRED")

    def test_scoring_refs_include_resolved_not_met_optional_criteria(self):
        packet = dashboard_packet()
        packet["criteria"][2].update({"status": "NOT_MET", "points_awarded": 0, "crosscheck_refs": ["cross-doc"]})
        packet["crosschecks"] = [{"id": "cross-doc", "order": 0, "subject_type": "NEGATIVE_FINDING", "subject_ref": "result-doc", "status": "CONFIRMED", "authority_refs": [rubric_ref("S1")], "evidence_refs": ["ev-readme"], "owner_id": "reviewer", "conclusion": "Tidak terpenuhi"}]
        packet["decision"]["scoring"].update({"value": 3, "included_criterion_refs": ["result-advanced"], "calculation": "Satu saran terpenuhi"})
        self.assert_invalid(packet, "SCORING_INCLUDED_REFS_MISMATCH")

    def test_runtime_owner_must_match_an_active_role_assignment(self):
        packet = dashboard_packet()
        packet["runtime_results"][0]["owner_id"] = "unknown-owner"
        self.assert_invalid(packet, "RUNTIME_OWNER_MISMATCH")

    def test_runtime_owner_role_must_have_compatible_capability(self):
        packet = dashboard_packet()
        packet["runtime_results"][0]["owner_id"] = "agent-files"
        packet["evidence"][4]["browser_trace"]["owner_id"] = "agent-files"
        self.assert_invalid(packet, "RUNTIME_OWNER_CAPABILITY_MISMATCH")

    def test_runtime_target_must_match_owner_exclusive_resource(self):
        packet = dashboard_packet()
        packet["runtime_results"][0]["target_resource"] = "browser:9333"
        self.assert_invalid(packet, "RUNTIME_TARGET_MISMATCH")

    def test_browser_trace_owner_must_match_runtime_owner(self):
        packet = dashboard_packet()
        packet["evidence"][4]["browser_trace"]["owner_id"] = "agent-files"
        self.assert_invalid(packet, "BROWSER_TRACE_OWNER_MISMATCH")

    def test_browser_trace_target_must_match_runtime_target(self):
        packet = dashboard_packet()
        packet["evidence"][4]["browser_trace"]["target_resource"] = "browser:9333"
        self.assert_invalid(packet, "BROWSER_TRACE_TARGET_MISMATCH")

    def test_irrelevant_unavailable_capability_does_not_block(self):
        profile = dashboard_profile()
        packet = dashboard_packet()
        packet["capabilities"]["items"].append({"id": "cap-external", "order": 2, "kind": "EXTERNAL_SERVICE", "status": "UNAVAILABLE", "reason": "Tidak digunakan profil"})
        seal_packet(profile, packet)
        self.assertEqual({"status": "READY", "exit_code": 0, "issues": []}, self.audit(profile, packet))

    def test_optional_count_uses_profile_tier_value(self):
        profile = dashboard_profile()
        profile["scoring"]["maximum_score"] = 5
        profile["scoring"]["tiers"][-1]["value"] = 5
        packet = seal_packet(profile, dashboard_packet())
        self.assert_invalid(packet, "SCORING_VALUE_MISMATCH", profile)

    def test_local_profile_reference_digest_is_verified(self):
        with tempfile.TemporaryDirectory() as directory:
            rubric = Path(directory) / "rubric.md"
            rubric.write_text("Rubric content", encoding="utf-8")
            profile = dashboard_profile()
            profile["authority_sources"][0].update({"reference": str(rubric), "sha256": H})
            packet = seal_packet(profile, dashboard_packet())
            result = self.audit(profile, packet)
            self.assertIn("REFERENCE_DIGEST_MISMATCH", self.codes(result))

    def test_local_evidence_reference_digest_is_verified(self):
        with tempfile.TemporaryDirectory() as directory:
            notebook = Path(directory) / "analysis.ipynb"
            notebook.write_text("{}", encoding="utf-8")
            packet = dashboard_packet()
            packet["evidence"][1].update({"source_reference": str(notebook), "sha256": H})
            self.assert_invalid(packet, "REFERENCE_DIGEST_MISMATCH")

    def test_local_raw_automation_reference_digest_is_verified(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "raw.log"
            output.write_text("PASS", encoding="utf-8")
            packet = dashboard_packet()
            packet["automation_runs"][0]["raw_output"].update({"reference": str(output), "sha256": H})
            self.assert_invalid(packet, "REFERENCE_DIGEST_MISMATCH")

    def test_validator_run_binds_audited_packet_digest(self):
        packet = dashboard_packet()
        packet["audit"]["validator_runs"][-1]["packet_sha256"] = H
        self.assert_invalid(packet, "VALIDATOR_PACKET_HASH_MISMATCH")

    def test_validator_run_binds_actual_profile_digest(self):
        packet = dashboard_packet()
        packet["audit"]["validator_runs"][-1]["profile_sha256"] = H
        self.assert_invalid(packet, "VALIDATOR_PROFILE_HASH_MISMATCH")

    def test_validator_run_requires_real_command(self):
        packet = dashboard_packet()
        packet["audit"]["validator_runs"][-1]["command"] = "TODO"
        self.assert_invalid(packet, "VALIDATOR_COMMAND_INVALID")

    def test_validator_run_result_must_be_ready_and_zero_issue(self):
        packet = dashboard_packet()
        packet["audit"]["validator_runs"][-1].update({"result": "INVALID", "issue_count": 1})
        self.assert_invalid(packet, "FRESH_VALIDATOR_RESULT_REQUIRED")

    def test_rubric_reference_requires_precise_locator(self):
        packet = dashboard_packet()
        packet["criteria"][0]["rubric_refs"] = ["rubric"]
        self.assert_invalid(packet, "SCHEMA_VALIDATION")

    def test_browser_trace_requires_browser_related_raw_provenance(self):
        packet = dashboard_packet()
        packet["evidence"][4]["derived_from_refs"] = ["ev-auto-raw"]
        self.assert_invalid(packet, "INVALID_BROWSER_PROVENANCE")

    def test_artifact_trigger_does_not_require_runtime_when_artifact_missing(self):
        profile = dashboard_profile()
        profile["artifact_types"].append({"id": "deployment-url", "order": 2, "title": "Deployment URL", "required": False, "accepted_extensions": [".txt"]})
        profile["runtime_obligations"].append({"id": "deployment-browser", "order": 1, "title": "Deployment", "kind": "browser_interaction", "required": True, "browser_trace_required": True, "trigger": {"type": "ARTIFACT_PRESENT", "artifact_type_ref": "deployment-url"}})
        packet = dashboard_packet()
        packet["submission_artifacts"].append({"id": "submitted-deployment", "order": 2, "profile_artifact_type_id": "deployment-url", "status": "MISSING", "evidence_refs": ["ev-note"], "conclusion": "Tidak disertakan"})
        seal_packet(profile, packet)
        self.assertEqual("READY", self.audit(profile, packet)["status"])

    def test_artifact_trigger_requires_runtime_when_artifact_present(self):
        profile = dashboard_profile()
        profile["artifact_types"].append({"id": "deployment-url", "order": 2, "title": "Deployment URL", "required": False, "accepted_extensions": [".txt"]})
        profile["runtime_obligations"].append({"id": "deployment-browser", "order": 1, "title": "Deployment", "kind": "browser_interaction", "required": True, "browser_trace_required": True, "trigger": {"type": "ARTIFACT_PRESENT", "artifact_type_ref": "deployment-url"}})
        packet = dashboard_packet()
        packet["submission_artifacts"].append({"id": "submitted-deployment", "order": 2, "profile_artifact_type_id": "deployment-url", "status": "PRESENT", "evidence_refs": ["ev-source"], "conclusion": "Disertakan"})
        seal_packet(profile, packet)
        self.assert_invalid(packet, "MISSING_RUNTIME_RESULT", profile)

    def test_explicit_capability_block_returns_exit_two(self):
        packet = dashboard_packet()
        packet["decision"]["status"] = "BLOCKED"
        packet["decision"]["scoring"].update({"eligible": False, "value": None, "maximum": None, "included_criterion_refs": [], "calculation": "Blocked"})
        packet["artifacts"][0].update({"kind": "REVIEWER_NOTE", "audience": "INTERNAL", "template_id": "blocked", "status": "BLOCKED", "decision_label": "BLOCKED"})
        packet["audit"].update({"workflow_state": "BLOCKED", "blocked_reason": "Browser unavailable", "manual_signoff": {"signed": False, "reviewer_id": "reviewer"}})
        packet["capabilities"]["items"][1].update({"status": "UNAVAILABLE", "reason": "Browser cannot start"})
        packet["runtime_results"][0]["status"] = "BLOCKED"
        packet["criteria"][1]["status"] = "BLOCKED"
        packet["criteria"][1]["crosscheck_refs"] = ["cross-browser"]
        packet["crosschecks"] = [{"id": "cross-browser", "order": 0, "subject_type": "AMBIGUOUS_FINDING", "subject_ref": "result-dashboard", "status": "BLOCKED", "authority_refs": [rubric_ref("K2")], "evidence_refs": ["ev-source"], "owner_id": "reviewer", "conclusion": "Browser unavailable"}]
        result = self.audit(packet=packet)
        self.assertEqual("BLOCKED", result["status"])
        self.assertEqual(2, result["exit_code"])

    def test_profile_schema_violation_is_reported(self):
        profile = dashboard_profile()
        profile["unexpected"] = True
        result = self.audit(profile, dashboard_packet())
        self.assertIn("SCHEMA_VALIDATION", self.codes(result))

    def test_packet_schema_violation_is_reported(self):
        packet = dashboard_packet()
        del packet["submission"]["id"]
        self.assert_invalid(packet, "SCHEMA_VALIDATION")

    def test_malformed_nested_value_returns_invalid_instead_of_crashing(self):
        packet = dashboard_packet()
        packet["criteria"][0] = "malformed"
        result = self.audit(packet=packet)
        self.assertEqual("INVALID", result["status"])
        self.assertIn("SCHEMA_VALIDATION", self.codes(result))

    def test_unavailable_capability_requires_reason(self):
        packet = dashboard_packet()
        packet["capabilities"]["items"][1]["status"] = "UNAVAILABLE"
        self.assert_invalid(packet, "CAPABILITY_REASON_REQUIRED")

    def test_cli_emits_json_and_expected_exit_code(self):
        with tempfile.TemporaryDirectory() as directory:
            profile_path = Path(directory) / "profile.json"
            packet_path = Path(directory) / "packet.json"
            profile_path.write_text(json.dumps(dashboard_profile()), encoding="utf-8")
            packet_path.write_text(json.dumps(dashboard_packet()), encoding="utf-8")
            completed = subprocess.run(
                [sys.executable, str(AUDITOR_PATH), "--profile", str(profile_path), "--packet", str(packet_path)],
                check=False, capture_output=True, text=True,
            )
            self.assertEqual(0, completed.returncode)
            self.assertEqual("READY", json.loads(completed.stdout)["status"])


if __name__ == "__main__":
    unittest.main()
