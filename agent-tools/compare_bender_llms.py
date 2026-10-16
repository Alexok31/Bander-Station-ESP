"""Small, reproducible voice-reply comparison; reads keys from local config only.

No saved chat, memories, recordings, or keys are written to the result file.
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

OUT = ROOT / "artifacts/bender-llm-comparison.json"
QUESTIONS = (
    "Привіт, Бендер. Як справи?",
    "Скільки буде сім на вісім?",
    "Завтра понеділок, а я вже втомився. Що робитимемо?",
    "Я спалив вечерю. Придумай смішне виправдання.",
    "Чому небо синє? Поясни просто.",
    "Мені сумно. Скажи щось по-людськи, без кліше.",
)
PROFILES = {
    "normal": dict(sarcasm=65, sociability=65, curiosity=30, stubbornness=40,
                   warmth=30, roughness=45, profanity=0),
    "rude": dict(sarcasm=80, sociability=65, curiosity=15, stubbornness=90,
                 warmth=10, roughness=100, profanity=100),
}
MODELS = ("grok-4.3", "claude-sonnet-5-5", "claude-haiku-5-5", "gpt-6-luna")


async def ask(client, model, profile, question):
    anthropic = model.startswith("claude-")
    openai = model.startswith("gpt-")
    prompt = (server.anthropic_bender_prompt(profile) if anthropic
              else server.bender_prompt(profile))
    if anthropic:
        body = {"model": model, "system": prompt,
                "messages": [{"role": "user", "content": question}],
                "stream": True, "max_tokens": 512,
                "output_config": {"effort": "low"}}
        if model == "claude-sonnet-5-5":
            body["thinking"] = {"type": "between_tools"}
        headers = {"x-api-key": server.ANTHROPIC_API_KEY,
                   "anthropic-version": "2023-06-01"}
        url = server.ANTHROPIC_MESSAGES_URL
    elif openai:
        body = {"model": model, "messages": [
            {"role": "system", "content": prompt},
            {"role": "user", "content": question}],
            "reasoning_effort": "none", "max_completion_tokens": 512,
            "stream": True, "stream_options": {"include_usage": True}}
        headers = {"Authorization": "Bearer " + server.OPENAI_API_KEY}
        url = server.OPENAI_CHAT_URL
    else:
        body = {"model": model, "instructions": prompt,
                "input": [{"role": "user", "content": question}],
                "stream": True, "store": False, "max_output_tokens": 512,
                "reasoning": {"effort": "none"}, "temperature": 0.68}
        headers = {"Authorization": "Bearer " + server.XAI_API_KEY}
        url = server.XAI_RESP_URL

    started = time.perf_counter()
    first_text = first_sentence = None
    answer = ""
    usage = {}
    async with client.stream("POST", url, headers=headers, json=body) as response:
        if response.status_code >= 400:
            raise RuntimeError(f"{model} HTTP {response.status_code}: " +
                               (await response.aread()).decode("utf-8", "replace")[:300])
        async for event in server._iter_sse(response):
            if event.get("type") == "error" or event.get("error"):
                raise RuntimeError(f"{model} stream error")
            if anthropic:
                if event.get("type") == "message_start":
                    usage.update((event.get("message") or {}).get("usage") or {})
                if event.get("type") == "message_delta":
                    usage.update(event.get("usage") or {})
                delta = event.get("delta") or {}
                piece = (delta.get("text", "") if event.get("type") == "content_block_delta"
                         and delta.get("type") == "text_delta" else "")
            elif openai:
                piece = "".join((choice.get("delta") or {}).get("content") or ""
                                for choice in event.get("choices") or [])
                usage = event.get("usage") or usage
            else:
                piece = server._grok_delta_text(event)
                usage = (event.get("response") or {}).get("usage") or usage
            if not piece:
                continue
            elapsed = round((time.perf_counter() - started) * 1000)
            if first_text is None:
                first_text = elapsed
            answer += piece
            if first_sentence is None and server._pop_sentences(answer)[0]:
                first_sentence = elapsed
    return {"model": model, "profile": profile_name(profile), "question": question,
            "answer": answer.strip(), "first_text_ms": first_text,
            "first_sentence_ms": first_sentence,
            "total_ms": round((time.perf_counter() - started) * 1000),
            "usage": usage}


def profile_name(profile):
    return "rude" if profile["profanity"] else "normal"


async def main():
    if not server.ANTHROPIC_API_KEY or not server.XAI_API_KEY or not server.OPENAI_API_KEY:
        raise SystemExit("Anthropic, xAI and OpenAI keys are required in local config")
    rows = json.loads(OUT.read_text(encoding="utf-8")) if OUT.exists() else []
    smoke = "--smoke" in sys.argv
    async with server._httpx_async(timeout=60.0, verify=ssl.create_default_context()) as client:
        for name, profile in PROFILES.items():
            for question in QUESTIONS:
                for model in MODELS:
                    if any(row["model"] == model and row["profile"] == name
                           and row["question"] == question for row in rows):
                        continue
                    row = await ask(client, model, profile, question)
                    rows.append(row)
                    OUT.parent.mkdir(parents=True, exist_ok=True)
                    OUT.write_text(json.dumps(rows, ensure_ascii=False, indent=2), encoding="utf-8")
                    print(model, name, row["first_sentence_ms"], row["total_ms"], flush=True)
                    if smoke:
                        return


if __name__ == "__main__":
    asyncio.run(main())
