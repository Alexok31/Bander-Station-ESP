"""Compare Claude and Grok on synthetic Ukrainian dialogue without user chat or audio.

Run with the same Python environment as local_server/start.bat. The script uses
configured API keys, makes a small number of paid requests, and saves responses
under artifacts/. It never sends saved chat, device memory, or recordings.
"""
import asyncio
import json
from pathlib import Path
import ssl
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "BendeRadio/examples/BenderRealtime/local_server"))
import server

OUT = ROOT / "artifacts/claude-grok-comparison.json"
QUESTIONS = (
    "Скільки буде сім на вісім?",
    "Завтра знову понеділок, а я вже втомився. Що робитимемо?",
    "Я спалив вечерю. Придумай смішне виправдання.",
    "Серйозно: чому небо синє? Поясни просто.",
)
PROFILES = {
    "normal": dict(sarcasm=65, sociability=65, curiosity=30, stubbornness=40,
                   warmth=30, roughness=45, profanity=0),
    "rude": dict(sarcasm=80, sociability=65, curiosity=15, stubbornness=90,
                 warmth=10, roughness=100, profanity=100),
}


async def ask(client, provider, prompt, history, question):
    model = server.ANTHROPIC_MODEL if provider == "anthropic" else server.GROK_MODEL
    turns = history + [{"role": "user", "content": question}]
    if provider == "anthropic":
        body = {"model": model, "system": prompt, "messages": turns,
                "stream": True, "max_tokens": 384}
        if model == "claude-sonnet-5-5":
            body["thinking"] = {"type": "between_tools"}
            body["output_config"] = {"effort": "low"}
        headers = {"x-api-key": server.ANTHROPIC_API_KEY,
                   "anthropic-version": "2023-06-01"}
        url = server.ANTHROPIC_MESSAGES_URL
    else:
        body = {"model": model, "instructions": prompt, "input": turns,
                "stream": True, "store": False, "temperature": 0.72,
                "max_output_tokens": 1024}
        if model == "grok-4.3":
            body["reasoning"] = {"effort": "none"}
        elif model in ("grok-4.5", "grok-4.6", "grok-4.7"):
            body["reasoning"] = {"effort": "low"}
        headers = {"Authorization": "Bearer " + server.XAI_API_KEY}
        url = server.XAI_RESP_URL
    started = time.perf_counter()
    first_text = first_sentence = None
    answer = ""
    usage = {}
    async with client.stream("POST", url, headers=headers, json=body) as response:
        if response.status_code >= 400:
            raise RuntimeError(f"{provider} HTTP {response.status_code}")
        async for event in server._iter_sse(response):
            if event.get("type") == "error" or event.get("error"):
                raise RuntimeError(f"{provider} stream error")
            if provider == "anthropic":
                delta = event.get("delta") or {}
                piece = delta.get("text", "") if event.get("type") == "content_block_delta" and delta.get("type") == "text_delta" else ""
                if event.get("type") == "message_delta":
                    usage = event.get("usage") or usage
            else:
                piece = server._grok_delta_text(event)
                usage = (event.get("response") or {}).get("usage") or usage
            if piece:
                elapsed = round((time.perf_counter() - started) * 1000)
                first_text = elapsed if first_text is None else first_text
                answer += piece
                sentences, _ = server._pop_sentences(answer)
                if sentences and first_sentence is None:
                    first_sentence = elapsed
    return {"model": model, "provider": provider, "question": question,
            "answer": answer.strip(), "first_text_ms": first_text,
            "first_sentence_ms": first_sentence,
            "total_ms": round((time.perf_counter() - started) * 1000),
            "usage": usage}


async def main():
    if not server.ANTHROPIC_API_KEY or not server.XAI_API_KEY:
        raise SystemExit("Both Anthropic and xAI keys are needed in server configuration")
    rows = json.loads(OUT.read_text(encoding="utf-8")) if OUT.exists() else []
    limit = 1 if "--smoke" in sys.argv else None
    requested = 0
    async with server._httpx_async(timeout=60.0, verify=ssl.create_default_context()) as client:
        for profile_name, profile in PROFILES.items():
            # Identical concise instructions and questions for both providers.
            prompt = server.local_bender_prompt(profile)
            histories = {"anthropic": [], "grok": []}
            for question in QUESTIONS:
                for provider in histories:
                    existing = next((row for row in rows if row["profile"] == profile_name
                                     and row["provider"] == provider and row["question"] == question
                                     and row["model"] == (server.ANTHROPIC_MODEL if provider == "anthropic"
                                                          else server.GROK_MODEL)), None)
                    if existing:
                        histories[provider].extend([
                            {"role": "user", "content": question},
                            {"role": "assistant", "content": existing["answer"]},
                        ])
                        continue
                    row = await ask(client, provider, prompt, histories[provider], question)
                    row["profile"] = profile_name
                    rows.append(row)
                    histories[provider].extend([
                        {"role": "user", "content": question},
                        {"role": "assistant", "content": row["answer"]},
                    ])
                    OUT.parent.mkdir(parents=True, exist_ok=True)
                    OUT.write_text(json.dumps(rows, ensure_ascii=False, indent=2), encoding="utf-8")
                    print(profile_name, provider, row["first_sentence_ms"], row["total_ms"], flush=True)
                    requested += 1
                    if limit is not None and requested >= limit:
                        return


if __name__ == "__main__":
    asyncio.run(main())
