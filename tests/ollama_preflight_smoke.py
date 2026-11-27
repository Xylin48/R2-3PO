#!/usr/bin/env python3
"""Unit smoke tests for the local Ollama readiness probe; never opens a socket."""
from __future__ import annotations

import base64
import contextlib
import io
import json
import sys
import unittest
import urllib.error
from types import SimpleNamespace
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import preflight_ollama_model as probe  # noqa: E402


class OllamaPreflightSmoke(unittest.TestCase):
    def test_context_falls_back_when_memory_cannot_be_measured(self) -> None:
        with patch("builtins.open", side_effect=OSError("simulated unavailable memory metrics")):
            self.assertEqual(probe._adaptive_context_tokens(), 4096)

    def test_unreadable_cgroup_usage_uses_conservative_context_tier(self) -> None:
        def fake_open(path: str, mode: str = "r", encoding: str = "ascii"):
            if path == "/proc/meminfo":
                return io.StringIO("MemAvailable: 16777216 kB\\n")
            if path == "/sys/fs/cgroup/memory.max":
                return io.StringIO(str(4 * 1024 ** 3))
            if path == "/sys/fs/cgroup/memory.current":
                raise OSError("simulated unreadable cgroup usage")
            raise OSError("path unavailable")

        with patch("builtins.open", side_effect=fake_open):
            self.assertEqual(probe._adaptive_context_tokens(), 2048)

    def test_unlimited_v2_takes_precedence_over_v1(self) -> None:
        def fake_open(path, mode="r", encoding="ascii"):
            values = {
                "/proc/meminfo": "MemAvailable: 16777216 kB\n",
                "/sys/fs/cgroup/memory.max": "max\n",
                "/sys/fs/cgroup/memory/memory.limit_in_bytes": str(2 * 1024 ** 3),
                "/sys/fs/cgroup/memory/memory.usage_in_bytes": "0",
            }
            if path not in values:
                raise OSError("path unavailable")
            return io.StringIO(values[path])
        with patch("builtins.open", side_effect=fake_open) as opened:
            self.assertEqual(probe._adaptive_context_tokens(), 16384)
            self.assertNotIn("/sys/fs/cgroup/memory/memory.limit_in_bytes",
                             [call.args[0] for call in opened.call_args_list])

    def test_runtime_decimal_policy_matches_actual_c_request(self) -> None:
        tags = {"models": [{"name": probe.MODEL}, {"name": probe.VISION_MODEL}]}
        text = {"done": True, "message": {"role": "assistant", "content": "R2_READY"}}
        image = {"done": True, "message": {"role": "assistant", "content": '{"color":"blue"}'}}
        for malformed in ("8_192", "8192 ", "８１９２"):
            with self.subTest(context=malformed), patch.dict(probe.os.environ, {
                "R2_OLLAMA_NUM_CTX": malformed, "R2_OLLAMA_NUM_GPU": "0",
            }), patch.object(probe, "_adaptive_context_tokens", return_value=4096), \
                 patch.object(probe, "_read_json", side_effect=[tags, text, image, {}]) as read, \
                 patch.object(probe.subprocess, "run", side_effect=FileNotFoundError):
                self.assertTrue(probe.preflight(timeout=1))
                for call in read.call_args_list[1:3]:
                    self.assertEqual(call.kwargs["payload"]["options"]["num_ctx"], 4096)
        for malformed in ("2_0", "20 ", "２０"):
            with self.subTest(gpu=malformed), patch.dict(probe.os.environ, {
                "R2_OLLAMA_NUM_GPU": malformed,
            }), patch.object(probe, "_read_json", return_value=tags) as read, \
                 contextlib.redirect_stderr(io.StringIO()):
                self.assertFalse(probe.preflight(timeout=1))
                self.assertEqual(read.call_count, 1)
        self.assertEqual(probe._runtime_decimal(" \t+20"), 20)
        self.assertEqual(probe._runtime_decimal("-1"), -1)

    def test_model_and_real_chat_completion_are_required(self) -> None:
        replies = [
            {"models": [{"name": "llama3.1:8b"}, {"name": "qwen3-vl:4b-instruct-q4_K_M"}]},
            {"done": True, "message": {"role": "assistant", "content": "R2_READY"}},
            {"done": True, "message": {"role": "assistant", "content": "{\"color\":\"blue\"}"}},
            {"models": [{"name": "qwen3-vl:4b-instruct-q4_K_M", "size_vram": 123456}]},
        ]
        gpu_result = SimpleNamespace(
            returncode=0, stdout="Test GPU, 4096 MiB, 123 MiB", stderr=""
        )
        with patch.object(probe, "_read_json", side_effect=replies) as read, \
             patch.object(probe.subprocess, "run", return_value=gpu_result) as gpu:
            self.assertTrue(probe.preflight(timeout=1))
        self.assertEqual(read.call_count, 4)
        self.assertTrue(read.call_args_list[0].args[0].endswith("/api/tags"))
        self.assertTrue(read.call_args_list[1].args[0].endswith("/api/chat"))
        payload = read.call_args_list[1].kwargs["payload"]
        self.assertEqual(payload["model"], "llama3.1:8b")
        self.assertIs(payload["think"], False)
        self.assertEqual([message["role"] for message in payload["messages"]],
                         ["system", "user", "assistant", "user"])
        self.assertIn("R2_READY", payload["messages"][1]["content"])
        self.assertNotIn("R2_READY", payload["messages"][-1]["content"])
        self.assertIs(payload["truncate"], False)
        self.assertIs(payload["shift"], False)
        self.assertEqual(payload["options"]["num_predict"], 512 if payload["options"]["num_ctx"] < 4096 else 1024)
        self.assertEqual(payload["options"]["temperature"], 0.7)
        self.assertEqual(payload["options"]["top_p"], 0.8)
        self.assertEqual(payload["options"]["top_k"], 20)
        self.assertEqual(payload["options"]["min_p"], 0.0)
        self.assertEqual(payload["options"]["presence_penalty"], 1.5)
        self.assertEqual(payload["options"]["repeat_penalty"], 1.0)
        self.assertEqual(payload["options"]["num_batch"], 128)
        self.assertTrue(read.call_args_list[2].args[0].endswith("/api/chat"))
        vision_payload = read.call_args_list[2].kwargs["payload"]
        self.assertIs(vision_payload["truncate"], False)
        self.assertIs(vision_payload["shift"], False)
        self.assertNotIn("num_batch", vision_payload["options"])
        png_bytes = base64.b64decode(vision_payload["messages"][1]["images"][0])
        self.assertEqual(png_bytes[:8], bytes((137, 80, 78, 71, 13, 10, 26, 10)))
        self.assertEqual(int.from_bytes(png_bytes[16:20], "big"), 640)
        self.assertEqual(int.from_bytes(png_bytes[20:24], "big"), 480)
        self.assertEqual(vision_payload["format"], "json")
        self.assertEqual(vision_payload["model"], "qwen3-vl:4b-instruct-q4_K_M")
        self.assertEqual(vision_payload["messages"][0]["role"], "system")
        self.assertEqual(vision_payload["options"]["num_predict"], 512 if vision_payload["options"]["num_ctx"] >= 8192 else 256)
        self.assertEqual(vision_payload["options"]["temperature"], 0.7)
        self.assertEqual(vision_payload["options"]["top_p"], 0.8)
        self.assertEqual(vision_payload["options"]["top_k"], 20)
        self.assertEqual(vision_payload["options"]["min_p"], 0.0)
        self.assertEqual(vision_payload["options"]["presence_penalty"], 1.5)
        self.assertEqual(vision_payload["options"]["repeat_penalty"], 1.0)
        self.assertTrue(read.call_args_list[3].args[0].endswith("/api/ps"))
        self.assertEqual(gpu.call_count, 1)

    def test_explicit_gpu_and_context_policy_reaches_real_chat_request(self) -> None:
        replies = [
            {"models": [{"name": "llama3.1:8b"}, {"name": "qwen3-vl:4b-instruct-q4_K_M"}]},
            {"done": True, "message": {"role": "assistant", "content": "R2_READY"}},
            {"models": [{"name": "llama3.1:8b", "size": 500000, "size_vram": 123456}]},
            {"done": True, "message": {"role": "assistant", "content": "{\"color\":\"blue\"}"}},
            {"models": [{"name": "qwen3-vl:4b-instruct-q4_K_M", "size": 500000, "size_vram": 123456}]},
        ]
        with patch.dict(probe.os.environ, {
            "R2_OLLAMA_NUM_GPU": "20",
            "R2_OLLAMA_NUM_CTX": "8192",
        }), patch.object(probe, "_read_json", side_effect=replies) as read, \
             patch.object(probe.subprocess, "run", return_value=SimpleNamespace(
                 returncode=0, stdout="NVIDIA GeForce GTX 1050 Ti, 4096 MiB, 123 MiB", stderr=""
             )):
            self.assertTrue(probe.preflight(timeout=1, require_gpu_offload=True))
        options = read.call_args_list[1].kwargs["payload"]["options"]
        self.assertEqual(options["num_gpu"], 20)
        self.assertEqual(options["num_ctx"], 8192)
        vision_payload = read.call_args_list[3].kwargs["payload"]
        self.assertIs(vision_payload["truncate"], False)
        self.assertIs(vision_payload["shift"], False)
        self.assertEqual(vision_payload["options"]["num_gpu"], 20)
        self.assertEqual(vision_payload["options"]["num_ctx"], 8192)
        self.assertEqual(vision_payload["options"]["num_predict"], 512)
        self.assertIn("images", vision_payload["messages"][1])

    def test_overnight_endpoint_is_used_for_tags_chat_and_process_checks(self) -> None:
        replies = [
            {"models": [{"name": "llama3.1:8b"}, {"name": "qwen3-vl:4b-instruct-q4_K_M"}]},
            {"done": True, "message": {"role": "assistant", "content": "R2_READY"}},
            {"done": True, "message": {"role": "assistant", "content": "{\"color\":\"blue\"}"}},
            {"models": [{"name": "qwen3-vl:4b-instruct-q4_K_M", "size_vram": 123456}]},
        ]
        gpu_result = SimpleNamespace(returncode=1, stdout="", stderr="")
        with patch.dict(probe.os.environ, {"R2_OLLAMA_BASE_URL": "http://127.0.0.1:11435"}), \
             patch.object(probe, "_read_json", side_effect=replies) as read, \
             patch.object(probe.subprocess, "run", return_value=gpu_result):
            self.assertTrue(probe.preflight(timeout=1))
        self.assertEqual(
            [call.args[0] for call in read.call_args_list],
            [
                "http://127.0.0.1:11435/api/tags",
                "http://127.0.0.1:11435/api/chat",
                "http://127.0.0.1:11435/api/chat",
                "http://127.0.0.1:11435/api/ps",
            ],
        )

    def test_required_gpu_offload_rejects_cpu_only_success(self) -> None:
        errors = io.StringIO()
        with patch.dict(probe.os.environ, {"R2_OLLAMA_NUM_GPU": "20"}), \
             patch.object(probe, "_read_json", side_effect=[
                 {"models": [{"name": "llama3.1:8b"}, {"name": "qwen3-vl:4b-instruct-q4_K_M"}]},
                 {"done": True, "message": {"role": "assistant", "content": "R2_READY"}},
                 {"models": [{"name": "llama3.1:8b", "size": 500000, "size_vram": 0}]},
                 {"done": True, "message": {"role": "assistant", "content": "{\"color\":\"blue\"}"}},
                 {"models": [{"name": "qwen3-vl:4b-instruct-q4_K_M", "size_vram": 0}]},
             ]), patch.object(probe.subprocess, "run", return_value=SimpleNamespace(
                 returncode=0, stdout="NVIDIA GeForce GTX 1050 Ti, 4096 MiB, 0 MiB", stderr=""
             )), contextlib.redirect_stderr(errors):
            self.assertFalse(probe.preflight(timeout=1, require_gpu_offload=True))
        self.assertIn("GPU offload was required", errors.getvalue())

    def test_automatic_layer_placement_passes_with_verified_offload(self) -> None:
        replies = [
            {"models": [{"name": "llama3.1:8b"}, {"name": "qwen3-vl:4b-instruct-q4_K_M"}]},
            {"done": True, "message": {"role": "assistant", "content": "R2_READY"}},
            {"models": [{"name": "llama3.1:8b", "size": 500000, "size_vram": 123456}]},
            {"done": True, "message": {"role": "assistant", "content": "{\"color\":\"blue\"}"}},
            {"models": [{"name": "qwen3-vl:4b-instruct-q4_K_M", "size": 500000, "size_vram": 123456}]},
        ]
        with patch.dict(probe.os.environ, {"R2_OLLAMA_NUM_GPU": "0", "R2_OLLAMA_NUM_CTX": "8192"}), \
             patch.object(probe, "_read_json", side_effect=replies) as read, \
             patch.object(probe.subprocess, "run", return_value=SimpleNamespace(
                 returncode=0, stdout="NVIDIA GeForce GTX 1050 Ti, 4096 MiB, 128 MiB", stderr=""
             )):
            self.assertTrue(probe.preflight(timeout=1, require_gpu_offload=True))
        self.assertNotIn("num_gpu", read.call_args_list[1].kwargs["payload"]["options"])
        self.assertEqual(read.call_args_list[3].kwargs["payload"]["options"]["num_predict"], 512)

    def test_fully_gpu_resident_vision_model_is_accepted(self) -> None:
        # A 4B Qwen visual model can fit in VRAM even when Llama is split.
        # Positive offload, not forced mixed placement, is the capability check.
        replies = [
            {"models": [{"name": probe.MODEL}, {"name": probe.VISION_MODEL}]},
            {"done": True, "message": {"role": "assistant", "content": "R2_READY"}},
            {"models": [{"name": probe.MODEL, "size": 500000, "size_vram": 123456}]},
            {"done": True, "message": {"role": "assistant", "content": '{"color":"blue"}'}},
            {"models": [{"name": probe.VISION_MODEL, "size": 123456, "size_vram": 123456}]},
        ]
        with patch.dict(probe.os.environ, {"R2_OLLAMA_NUM_GPU": "20"}), \
             patch.object(probe, "_read_json", side_effect=replies) as read, \
             patch.object(probe.subprocess, "run", return_value=SimpleNamespace(
                 returncode=0, stdout="GTX 1050 Ti, 4096 MiB, 100 MiB", stderr=""
             )):
            self.assertTrue(probe.preflight(timeout=1, require_gpu_offload=True))
        self.assertEqual(read.call_count, 5)

    def test_unapproved_non_loopback_endpoint_is_rejected(self) -> None:
        errors = io.StringIO()
        with patch.dict(probe.os.environ, {"R2_OLLAMA_BASE_URL": "http://example.com:11435"}), \
             patch.object(probe, "_read_json") as read:
            with contextlib.redirect_stderr(errors):
                self.assertFalse(probe.preflight(timeout=1))
        read.assert_not_called()
        self.assertIn("must be http://127.0.0.1:11434 or", errors.getvalue())

    def test_gpu_diagnostics_failure_does_not_block_ready_model(self) -> None:
        # GPU placement is diagnostic only; a successful real chat must remain
        # ready if /api/ps or nvidia-smi is unavailable.
        errors = io.StringIO()
        with patch.object(probe, "_read_json", side_effect=[
            {"models": [{"name": "llama3.1:8b"}, {"name": "qwen3-vl:4b-instruct-q4_K_M"}]},
            {"done": True, "message": {"role": "assistant", "content": "R2_READY"}},
            {"done": True, "message": {"role": "assistant", "content": "{\"color\":\"blue\"}"}},
            OSError("simulated /api/ps unavailable"),
        ]), patch.object(probe.subprocess, "run", side_effect=FileNotFoundError("nvidia-smi")):
            with contextlib.redirect_stderr(errors):
                self.assertTrue(probe.preflight(timeout=1))
        self.assertEqual(errors.getvalue(), "")

    def test_text_gpu_report_requires_only_valid_vram_and_a_model_list(self) -> None:
        cases = [
            ({"models": [{"name": probe.MODEL, "size_vram": 123456, "size": None}]}, True),
            ({"models": None}, False),
            ({"models": 123}, False),
        ]
        for processes, expected in cases:
            with self.subTest(processes=processes), patch.object(probe, "_read_json", side_effect=[
                {"models": [{"name": probe.MODEL}, {"name": probe.VISION_MODEL}]},
                {"done": True, "message": {"role": "assistant", "content": "R2_READY"}},
                processes,
                {"done": True, "message": {"role": "assistant", "content": '{"color":"blue"}'}},
                {"models": [{"name": probe.VISION_MODEL, "size_vram": 123456}]},
            ]), patch.object(probe.subprocess, "run", side_effect=FileNotFoundError("nvidia-smi")):
                self.assertEqual(probe.preflight(timeout=1, require_gpu_offload=True), expected)

    def test_loopback_requests_bypass_http_proxy_environment(self) -> None:
        with patch.dict("os.environ", {"http_proxy": "http://127.0.0.1:9", "no_proxy": ""}):
            response = io.StringIO('{"models":[]}')
            response.code, response.msg, response.info = 200, "OK", lambda: {}
            with patch("urllib.request.HTTPHandler.http_open", return_value=response) as request:
                self.assertEqual(probe._read_json(probe.DEFAULT_API_BASE + "/api/tags", 1), {"models": []})
            self.assertEqual(request.call_args.args[0].host, "127.0.0.1:11434")

    def test_wrong_color_json_blocks_ready_status(self) -> None:
        errors = io.StringIO()
        red_literal = json.dumps({"color": "red"})
        with patch.object(probe, "_read_json", side_effect=[
            {"models": [{"name": "llama3.1:8b"}, {"name": "qwen3-vl:4b-instruct-q4_K_M"}]},
            {"done": True, "message": {"role": "assistant", "content": "R2_READY"}},
            {"done": True, "message": {"role": "assistant", "content": red_literal}},
        ]) as read, patch.object(probe.subprocess, "run", return_value=SimpleNamespace(
            returncode=0, stdout="Test GPU, 4096 MiB, 123 MiB", stderr=""
        )), contextlib.redirect_stderr(errors):
            self.assertFalse(probe.preflight(timeout=1))
        self.assertEqual(read.call_count, 3)
        self.assertIn("failed the JSON known-color vision probe", errors.getvalue())

    def test_image_inference_failure_blocks_ready_status(self) -> None:
        errors = io.StringIO()
        with patch.object(probe, "_read_json", side_effect=[
            {"models": [{"name": "llama3.1:8b"}, {"name": "qwen3-vl:4b-instruct-q4_K_M"}]},
            {"done": True, "message": {"role": "assistant", "content": "R2_READY"}},
            OSError("simulated CUDA out of memory during image inference"),
        ]) as read, patch.object(probe.subprocess, "run", return_value=SimpleNamespace(
            returncode=0, stdout="Test GPU, 4096 MiB, 123 MiB", stderr=""
        )), contextlib.redirect_stderr(errors):
            self.assertFalse(probe.preflight(timeout=1))
        self.assertEqual(read.call_count, 3)
        self.assertIn("image-bearing chat failed", errors.getvalue())
        self.assertIn("CUDA out of memory", errors.getvalue())

    def test_similarly_prefixed_model_is_not_the_installed_required_model(self) -> None:
        errors = io.StringIO()
        with patch.object(probe, "_read_json", return_value={
            "models": [{"name": "llama3.1:8b-custom"}, {"name": "llama3.1:8b-other"}]
        }) as read, contextlib.redirect_stderr(errors):
            self.assertFalse(probe.preflight(wait_seconds=0, timeout=1))
        self.assertEqual(read.call_count, 1)
        self.assertIn("both local models are required:", errors.getvalue())

    def test_similarly_prefixed_running_model_cannot_satisfy_gpu_offload(self) -> None:
        errors = io.StringIO()
        with patch.object(probe, "_read_json", side_effect=[
            {"models": [{"name": "llama3.1:8b"}, {"name": "qwen3-vl:4b-instruct-q4_K_M"}]},
            {"done": True, "message": {"role": "assistant", "content": "R2_READY"}},
            {"models": [{"name": "llama3.1:8b", "size": 500000, "size_vram": 123456}]},
            {"done": True, "message": {"role": "assistant", "content": '{"color":"blue"}'}},
            {"models": [{"name": "qwen3-vl:4b-instruct-q4_K_M-other",
                         "size": 500000, "size_vram": 123456}]},
        ]) as read, contextlib.redirect_stderr(errors):
            self.assertFalse(probe.preflight(timeout=1, require_gpu_offload=True))
        self.assertEqual(read.call_count, 5)
        self.assertIn("GPU offload was required", errors.getvalue())

    def test_missing_model_fails_without_waiting_or_network(self) -> None:
        errors = io.StringIO()
        with patch.object(probe, "_read_json", return_value={"models": []}) as read:
            with contextlib.redirect_stderr(errors):
                self.assertFalse(probe.preflight(wait_seconds=0, timeout=1))
        self.assertEqual(read.call_count, 1)
        self.assertIn("both local models are required:", errors.getvalue())

    def test_listed_model_without_chat_text_fails(self) -> None:
        errors = io.StringIO()
        with patch.object(probe, "_read_json", side_effect=[
            {"models": [{"name": "llama3.1:8b"}, {"name": "qwen3-vl:4b-instruct-q4_K_M"}]},
            {"message": {"content": "   "}},
        ]):
            with contextlib.redirect_stderr(errors):
                self.assertFalse(probe.preflight(timeout=1))
        self.assertIn("chat generation produced no usable final answer", errors.getvalue())

    def test_text_probe_does_not_replay_incomplete_answers(self) -> None:
        for content, reason in (("R2_READY", "length"), ("", "stop")):
            with self.subTest(reason=reason), patch.object(probe, "_read_json", side_effect=[
                {"models": [{"name": "llama3.1:8b"}, {"name": "qwen3-vl:4b-instruct-q4_K_M"}]},
                {"done": True, "done_reason": reason,
                 "message": {"role": "assistant", "content": content, "thinking": "internal"}},
            ]) as read, contextlib.redirect_stderr(io.StringIO()):
                self.assertFalse(probe.preflight(timeout=1))
                self.assertEqual(read.call_count, 2)

    def test_nonmatching_foreground_reply_does_not_pass_preflight(self) -> None:
        errors = io.StringIO()
        with patch.object(probe, "_read_json", side_effect=[
            {"models": [{"name": "llama3.1:8b"}, {"name": "qwen3-vl:4b-instruct-q4_K_M"}]},
            {"done": True, "message": {"role": "assistant", "content": "I am ready. R2_READY"}},
        ]) as read, contextlib.redirect_stderr(errors):
            self.assertFalse(probe.preflight(timeout=1))
        self.assertEqual(read.call_count, 2)
        self.assertIn("did not return its expected R2_READY marker", errors.getvalue())

    def test_incomplete_text_turn_does_not_pass_preflight(self) -> None:
        errors = io.StringIO()
        with patch.object(probe, "_read_json", side_effect=[
            {"models": [{"name": "llama3.1:8b"}, {"name": "qwen3-vl:4b-instruct-q4_K_M"}]},
            {"done": False, "message": {"role": "assistant", "content": "R2_READY"}},
        ]) as read, contextlib.redirect_stderr(errors):
            self.assertFalse(probe.preflight(timeout=1))
        self.assertEqual(read.call_count, 2)
        self.assertIn("did not return a complete assistant turn", errors.getvalue())

    def test_incomplete_vision_turn_does_not_pass_preflight(self) -> None:
        errors = io.StringIO()
        with patch.object(probe, "_read_json", side_effect=[
            {"models": [{"name": "llama3.1:8b"}, {"name": "qwen3-vl:4b-instruct-q4_K_M"}]},
            {"done": True, "message": {"role": "assistant", "content": "R2_READY"}},
            {"done": False, "message": {"role": "assistant", "content": "{\"color\":\"blue\"}"}},
        ]) as read, contextlib.redirect_stderr(errors):
            self.assertFalse(probe.preflight(timeout=1))
        self.assertEqual(read.call_count, 3)
        self.assertIn("incomplete or invalid assistant turn", errors.getvalue())

    def test_explicit_non_stop_completion_cannot_pass_either_probe(self) -> None:
        for vision in (False, True):
            for reason in ("length", "cancelled", "load", "", None):
                with self.subTest(vision=vision, reason=reason):
                    text = {"done": True, "message": {"role": "assistant", "content": "R2_READY"}}
                    image = {"done": True, "message": {"role": "assistant", "content": '{"color":"blue"}'}}
                    (image if vision else text)["done_reason"] = reason
                    replies = [{"models": [{"name": "llama3.1:8b"}, {"name": "qwen3-vl:4b-instruct-q4_K_M"}]}, text]
                    if vision:
                        replies.append(image)
                    with patch.object(probe, "_read_json", side_effect=replies) as read, \
                         contextlib.redirect_stderr(io.StringIO()):
                        self.assertFalse(probe.preflight(timeout=1))
                    self.assertEqual(read.call_count, 3 if vision else 2)

    def test_negated_blue_does_not_pass_the_vision_probe(self) -> None:
        with patch.object(probe, "_read_json", side_effect=[
            {"models": [{"name": "llama3.1:8b"}, {"name": "qwen3-vl:4b-instruct-q4_K_M"}]},
            {"done": True, "message": {"role": "assistant", "content": "R2_READY"}},
            {"done": True, "message": {"role": "assistant", "content": '{"color":"not blue"}'}},
        ]) as read, contextlib.redirect_stderr(io.StringIO()):
            self.assertFalse(probe.preflight(timeout=1))
        self.assertEqual(read.call_count, 3)

    def test_http_500_body_is_preserved_for_text_generation(self) -> None:
        errors = io.StringIO()
        failure = urllib.error.HTTPError(
            "http://127.0.0.1:11434/api/chat",
            500,
            "Internal Server Error",
            hdrs=None,
            fp=io.BytesIO(b'{"error":"CUDA out of memory while allocating KV cache"}'),
        )
        with patch.object(probe, "_read_json", side_effect=[
            {"models": [{"name": "llama3.1:8b"}, {"name": "qwen3-vl:4b-instruct-q4_K_M"}]},
            failure,
        ]), contextlib.redirect_stderr(errors):
            self.assertFalse(probe.preflight(timeout=1))
        self.assertIn("HTTP 500 Internal Server Error", errors.getvalue())
        self.assertIn("CUDA out of memory while allocating KV cache", errors.getvalue())

    def test_cli_identifies_only_reported_inference_capacity_errors(self) -> None:
        tags = {"models": [{"name": "llama3.1:8b"}, {"name": "qwen3-vl:4b-instruct-q4_K_M"}]}
        text = {"done": True, "message": {"role": "assistant", "content": "R2_READY"}}
        for preceding in ([tags], [tags, text]):
            for status, detail, expected in (
                (500, "CUDA out of memory allocating buffers", probe.CAPACITY_EXIT),
                (500, "image processor failed", 1),
                (400, "bad request contains out of memory", 1),
            ):
                failure = urllib.error.HTTPError(
                    "http://127.0.0.1:11434/api/chat", status, "Probe error", hdrs=None,
                    fp=io.BytesIO(('{"error":"' + detail + '"}').encode()),
                )
                with self.subTest(status=status, detail=detail, phase=len(preceding)), \
                     patch.object(probe.sys, "argv", ["preflight", "--timeout", "1"]), \
                     patch.object(probe, "_read_json", side_effect=[*preceding, failure]), \
                     contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(probe.main(), expected)

    def test_http_500_body_is_preserved_for_vision_generation(self) -> None:
        errors = io.StringIO()
        failure = urllib.error.HTTPError(
            "http://127.0.0.1:11434/api/chat",
            500,
            "Internal Server Error",
            hdrs=None,
            fp=io.BytesIO(b'{"error":"runner process has terminated"}'),
        )
        with patch.object(probe, "_read_json", side_effect=[
            {"models": [{"name": "llama3.1:8b"}, {"name": "qwen3-vl:4b-instruct-q4_K_M"}]},
            {"done": True, "message": {"role": "assistant", "content": "R2_READY"}},
            failure,
        ]), contextlib.redirect_stderr(errors):
            self.assertFalse(probe.preflight(timeout=1))
        self.assertIn("image-bearing chat failed", errors.getvalue())
        self.assertIn("runner process has terminated", errors.getvalue())

    def test_api_failure_is_reported_without_network_access(self) -> None:
        errors = io.StringIO()
        with patch.object(probe, "_read_json", side_effect=OSError("simulated offline API")):
            with contextlib.redirect_stderr(errors):
                self.assertFalse(probe.preflight(wait_seconds=0, timeout=1))
        self.assertIn("simulated offline API", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
