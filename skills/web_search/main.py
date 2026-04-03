#!/usr/bin/env python3
import os
import json
import urllib.parse
import urllib.request
import urllib.error
import html
import re
from typing import Any, Dict, List, Tuple

from runtime.sdk.python.velix_process import VelixProcess


class WebSearchSkill(VelixProcess):
    def __init__(self) -> None:
        super().__init__("web_search", "skill")

    def run(self) -> None:
        query = str(self.params.get("query", "")).strip()
        if not query:
            self._report_error("Missing required parameter: query")
            return

        max_results = self.params.get("max_results", 3)
        try:
            max_results = int(max_results)
        except Exception:
            max_results = 3
        max_results = max(1, min(max_results, 8))

        timeout_sec = self.params.get("timeout_sec", 8)
        try:
            timeout_sec = int(timeout_sec)
        except Exception:
            timeout_sec = 8
        timeout_sec = max(1, min(timeout_sec, 20))

        mode = str(self.params.get("mode", "sequential")).strip().lower()
        if mode not in {"sequential", "parallel"}:
            mode = "sequential"

        allow_firecrawl_scrape = bool(self.params.get("allow_firecrawl_scrape", False))

        requested_sources = self.params.get(
            "sources",
            ["brave", "tavily", "firecrawl", "wikipedia", "arxiv", "duckduckgo"],
        )
        if not isinstance(requested_sources, list):
            requested_sources = ["brave", "tavily", "firecrawl", "wikipedia", "arxiv", "duckduckgo"]
        requested_sources = [str(s).strip().lower() for s in requested_sources if str(s).strip()]

        pipeline_order = ["brave", "tavily", "firecrawl", "wikipedia", "arxiv", "duckduckgo"]
        enabled_order = [s for s in pipeline_order if s in set(requested_sources)]
        if not enabled_order:
            enabled_order = pipeline_order

        warnings: List[str] = []

        if mode == "parallel":
            provider_results, provider_diag = self._run_parallel_pipeline(
                query, max_results, timeout_sec, enabled_order, allow_firecrawl_scrape
            )
        else:
            provider_results, provider_diag = self._run_sequential_pipeline(
                query, max_results, timeout_sec, enabled_order, allow_firecrawl_scrape
            )

        merged = self._merge_and_dedup(provider_results, max(6, max_results * 3))
        final_results = merged[: max_results]

        result = {
            "status": "success",
            "query": query,
            "results": final_results,
            "summary": self._summarize_titles(final_results),
            "mode": mode,
            "pipeline_order": enabled_order,
            "providers": provider_diag,
            "sources_used": sorted({str(it.get("source", "")) for it in final_results if it.get("source")}),
            "warnings": warnings,
            "note": "Fallback pipeline uses official APIs first and DuckDuckGo as final backup.",
        }

        self.report_result(self.parent_pid, result, self.entry_trace_id)

    def _run_sequential_pipeline(
        self,
        query: str,
        limit: int,
        timeout_sec: int,
        ordered_sources: List[str],
        allow_firecrawl_scrape: bool,
    ) -> Tuple[Dict[str, List[Dict[str, Any]]], List[Dict[str, Any]]]:
        provider_results: Dict[str, List[Dict[str, Any]]] = {}
        diagnostics: List[Dict[str, Any]] = []

        for source in ordered_sources:
            items: List[Dict[str, Any]] = []
            failure_reason = ""
            try:
                items = self._search_source(source, query, limit, timeout_sec, allow_firecrawl_scrape)
                if not items:
                    failure_reason = "no_results"
            except Exception as exc:
                failure_reason = self._classify_failure(str(exc))

            provider_results[source] = items
            diagnostics.append(
                {
                    "source": source,
                    "count": len(items),
                    "status": "ok" if items else "fallback",
                    "reason": failure_reason,
                }
            )

            # Stop once primary source returns good results.
            if items:
                break

        return provider_results, diagnostics

    def _run_parallel_pipeline(
        self,
        query: str,
        limit: int,
        timeout_sec: int,
        ordered_sources: List[str],
        allow_firecrawl_scrape: bool,
    ) -> Tuple[Dict[str, List[Dict[str, Any]]], List[Dict[str, Any]]]:
        from concurrent.futures import ThreadPoolExecutor, as_completed

        provider_results: Dict[str, List[Dict[str, Any]]] = {s: [] for s in ordered_sources}
        diagnostics: List[Dict[str, Any]] = []

        with ThreadPoolExecutor(max_workers=min(6, len(ordered_sources))) as pool:
            futures = {
                pool.submit(self._search_source, s, query, limit, timeout_sec, allow_firecrawl_scrape): s
                for s in ordered_sources
            }
            for future in as_completed(futures):
                source = futures[future]
                try:
                    items = future.result()
                    provider_results[source] = items
                    diagnostics.append(
                        {
                            "source": source,
                            "count": len(items),
                            "status": "ok" if items else "fallback",
                            "reason": "" if items else "no_results",
                        }
                    )
                except Exception as exc:
                    diagnostics.append(
                        {
                            "source": source,
                            "count": 0,
                            "status": "fallback",
                            "reason": self._classify_failure(str(exc)),
                        }
                    )

        return provider_results, diagnostics

    def _search_source(
        self,
        source: str,
        query: str,
        limit: int,
        timeout_sec: int,
        allow_firecrawl_scrape: bool,
    ) -> List[Dict[str, Any]]:
        if source == "brave":
            return self._search_brave(query, limit, timeout_sec)
        if source == "tavily":
            return self._search_tavily(query, limit, timeout_sec)
        if source == "firecrawl":
            return self._search_firecrawl(query, limit, timeout_sec, allow_firecrawl_scrape)
        if source == "wikipedia":
            return self._search_wikipedia(query, limit, timeout_sec)
        if source == "arxiv":
            return self._search_arxiv(query, limit, timeout_sec)
        if source == "duckduckgo":
            return self._search_duckduckgo(query, limit, timeout_sec)
        return []

    def _search_brave(self, query: str, limit: int, timeout_sec: int) -> List[Dict[str, Any]]:
        api_key = os.getenv("BRAVE_API_KEY", "").strip()
        if not api_key:
            raise RuntimeError("api_error:missing_brave_api_key")

        params = {
            "q": query,
            "count": str(limit),
            "search_lang": "en",
            "country": "us",
            "safesearch": "moderate",
        }
        url = "https://api.search.brave.com/res/v1/web/search?" + urllib.parse.urlencode(params)
        payload = self._get_json(url, timeout_sec, headers={"X-Subscription-Token": api_key})

        out: List[Dict[str, Any]] = []
        for item in payload.get("web", {}).get("results", []):
            out.append(
                {
                    "source": "brave",
                    "title": str(item.get("title", "")).strip(),
                    "url": str(item.get("url", "")).strip(),
                    "snippet": str(item.get("description", "")).strip(),
                }
            )
        return self._sanitize_results(out)

    def _search_tavily(self, query: str, limit: int, timeout_sec: int) -> List[Dict[str, Any]]:
        api_key = os.getenv("TAVILY_API_KEY", "").strip()
        if not api_key:
            raise RuntimeError("api_error:missing_tavily_api_key")

        url = "https://api.tavily.com/search"
        payload = self._post_json(
            url,
            {
                "api_key": api_key,
                "query": query,
                "max_results": limit,
                "search_depth": "advanced",
                "include_answer": False,
                "include_raw_content": False,
            },
            timeout_sec,
        )

        out: List[Dict[str, Any]] = []
        for item in payload.get("results", []):
            out.append(
                {
                    "source": "tavily",
                    "title": str(item.get("title", "")).strip(),
                    "url": str(item.get("url", "")).strip(),
                    "snippet": str(item.get("content", "")).strip(),
                }
            )
        return self._sanitize_results(out)

    def _search_firecrawl(
        self, query: str, limit: int, timeout_sec: int, allow_scrape: bool
    ) -> List[Dict[str, Any]]:
        api_key = os.getenv("FIRECRAWL_API_KEY", "").strip()
        if not api_key:
            raise RuntimeError("api_error:missing_firecrawl_api_key")

        headers = {"Authorization": f"Bearer {api_key}"}
        payload = {
            "query": query,
            "limit": limit,
            "formats": ["markdown"] if allow_scrape else ["text"],
        }

        # Firecrawl API variants differ across deployments; try both known paths.
        last_error = "api_error"
        for endpoint in ("https://api.firecrawl.dev/v1/search", "https://api.firecrawl.dev/v0/search"):
            try:
                data = self._post_json(endpoint, payload, timeout_sec, headers=headers)
                out: List[Dict[str, Any]] = []
                for item in data.get("data", []) or data.get("results", []):
                    out.append(
                        {
                            "source": "firecrawl",
                            "title": str(item.get("title", "")).strip() or str(item.get("url", "")).strip(),
                            "url": str(item.get("url", "")).strip(),
                            "snippet": str(item.get("markdown", "")).strip()[:600]
                            if item.get("markdown")
                            else str(item.get("description", "")).strip(),
                        }
                    )
                return self._sanitize_results(out)
            except Exception as exc:
                last_error = self._classify_failure(str(exc))
        raise RuntimeError(last_error)

    def _search_wikipedia(self, query: str, limit: int, timeout_sec: int) -> List[Dict[str, Any]]:
        params = {
            "action": "query",
            "list": "search",
            "srsearch": query,
            "utf8": "1",
            "format": "json",
            "srlimit": str(limit),
        }
        url = "https://en.wikipedia.org/w/api.php?" + urllib.parse.urlencode(params)
        payload = self._get_json(url, timeout_sec)
        out: List[Dict[str, Any]] = []
        for item in payload.get("query", {}).get("search", []):
            title = str(item.get("title", "")).strip()
            page_url = "https://en.wikipedia.org/wiki/" + urllib.parse.quote(title.replace(" ", "_"))
            snippet = str(item.get("snippet", "")).replace("<span class=\"searchmatch\">", "").replace("</span>", "")
            out.append(
                {
                    "source": "wikipedia",
                    "title": title,
                    "url": page_url,
                    "snippet": snippet,
                }
            )
        return self._sanitize_results(out)

    def _search_arxiv(self, query: str, limit: int, timeout_sec: int) -> List[Dict[str, Any]]:
        params = {
            "search_query": f"all:{query}",
            "start": "0",
            "max_results": str(limit),
            "sortBy": "relevance",
            "sortOrder": "descending",
        }
        url = "http://export.arxiv.org/api/query?" + urllib.parse.urlencode(params)
        raw = self._get_text(url, timeout_sec)

        out: List[Dict[str, Any]] = []
        entries = re.findall(r"<entry>(.*?)</entry>", raw, flags=re.DOTALL)
        for block in entries[:limit]:
            title_match = re.search(r"<title>(.*?)</title>", block, flags=re.DOTALL)
            summary_match = re.search(r"<summary>(.*?)</summary>", block, flags=re.DOTALL)
            id_match = re.search(r"<id>(.*?)</id>", block, flags=re.DOTALL)
            title = html.unescape((title_match.group(1) if title_match else "").strip()).replace("\n", " ")
            snippet = html.unescape((summary_match.group(1) if summary_match else "").strip()).replace("\n", " ")
            url_entry = (id_match.group(1) if id_match else "").strip()
            out.append({"source": "arxiv", "title": title, "url": url_entry, "snippet": snippet})
        return self._sanitize_results(out)

    def _search_duckduckgo(self, query: str, limit: int, timeout_sec: int) -> List[Dict[str, Any]]:
        # Prefer ddgs package if present. Fallback to DuckDuckGo public instant-answer API.
        try:
            from duckduckgo_search import DDGS  # type: ignore

            out: List[Dict[str, Any]] = []
            with DDGS() as ddgs:
                for item in ddgs.text(query, max_results=limit):
                    out.append(
                        {
                            "source": "duckduckgo",
                            "title": str(item.get("title", "")).strip(),
                            "url": str(item.get("href", "")).strip(),
                            "snippet": str(item.get("body", "")).strip(),
                        }
                    )
            return self._sanitize_results(out)
        except Exception:
            pass

        params = {
            "q": query,
            "format": "json",
            "no_redirect": "1",
            "no_html": "1",
            "skip_disambig": "1",
        }
        url = "https://api.duckduckgo.com/?" + urllib.parse.urlencode(params)
        payload = self._get_json(url, timeout_sec)

        out: List[Dict[str, Any]] = []
        abstract = str(payload.get("AbstractText", "")).strip()
        heading = str(payload.get("Heading", "")).strip()
        abstract_url = str(payload.get("AbstractURL", "")).strip()
        if abstract and abstract_url:
            out.append(
                {
                    "source": "duckduckgo",
                    "title": heading or query,
                    "url": abstract_url,
                    "snippet": abstract,
                }
            )

        for topic in payload.get("RelatedTopics", []):
            if len(out) >= limit:
                break
            if isinstance(topic, dict) and "Topics" in topic and isinstance(topic["Topics"], list):
                for sub in topic["Topics"]:
                    if len(out) >= limit:
                        break
                    text = str(sub.get("Text", "")).strip()
                    first_url = str(sub.get("FirstURL", "")).strip()
                    if text and first_url:
                        out.append(
                            {
                                "source": "duckduckgo",
                                "title": text.split(" - ", 1)[0],
                                "url": first_url,
                                "snippet": text,
                            }
                        )
            elif isinstance(topic, dict):
                text = str(topic.get("Text", "")).strip()
                first_url = str(topic.get("FirstURL", "")).strip()
                if text and first_url:
                    out.append(
                        {
                            "source": "duckduckgo",
                            "title": text.split(" - ", 1)[0],
                            "url": first_url,
                            "snippet": text,
                        }
                    )

        return self._sanitize_results(out[:limit])

    def _get_json(self, url: str, timeout_sec: int, headers: Dict[str, str] = None) -> Dict[str, Any]:
        if headers is None:
            headers = {}
        req = urllib.request.Request(
            url,
            headers={
                "User-Agent": "VelixWebSearchSkill/1.0 (+https://example.invalid)",
                "Accept": "application/json",
                **headers,
            },
            method="GET",
        )
        with urllib.request.urlopen(req, timeout=timeout_sec) as resp:
            raw = resp.read().decode("utf-8", errors="replace")
            return json.loads(raw)

    def _post_json(
        self,
        url: str,
        payload: Dict[str, Any],
        timeout_sec: int,
        headers: Dict[str, str] = None,
    ) -> Dict[str, Any]:
        if headers is None:
            headers = {}
        body = json.dumps(payload).encode("utf-8")
        req = urllib.request.Request(
            url,
            data=body,
            headers={
                "User-Agent": "VelixWebSearchSkill/1.0 (+https://example.invalid)",
                "Accept": "application/json",
                "Content-Type": "application/json",
                **headers,
            },
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=timeout_sec) as resp:
            raw = resp.read().decode("utf-8", errors="replace")
            return json.loads(raw)

    def _get_text(self, url: str, timeout_sec: int) -> str:
        req = urllib.request.Request(
            url,
            headers={
                "User-Agent": "VelixWebSearchSkill/1.0 (+https://example.invalid)",
                "Accept": "application/atom+xml,text/xml;q=0.9,*/*;q=0.8",
            },
            method="GET",
        )
        with urllib.request.urlopen(req, timeout=timeout_sec) as resp:
            return resp.read().decode("utf-8", errors="replace")

    def _merge_and_dedup(
        self, provider_results: Dict[str, List[Dict[str, Any]]], limit: int
    ) -> List[Dict[str, Any]]:
        merged: List[Dict[str, Any]] = []
        seen = set()

        for source in ["brave", "tavily", "firecrawl", "wikipedia", "arxiv", "duckduckgo"]:
            for item in provider_results.get(source, []):
                key = (str(item.get("url", "")).strip().lower(), str(item.get("title", "")).strip().lower())
                if key in seen:
                    continue
                seen.add(key)
                merged.append(item)
                if len(merged) >= limit:
                    return merged
        return merged

    def _sanitize_results(self, items: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
        out: List[Dict[str, Any]] = []
        for item in items:
            title = str(item.get("title", "")).strip()
            url = str(item.get("url", "")).strip()
            snippet = str(item.get("snippet", "")).strip()
            if not title and not url:
                continue
            out.append(
                {
                    "source": str(item.get("source", "")).strip(),
                    "title": title,
                    "url": url,
                    "snippet": snippet,
                }
            )
        return out

    def _classify_failure(self, message: str) -> str:
        m = message.lower()
        if "429" in m or "rate" in m or "quota" in m:
            return "rate_limit"
        if "403" in m or "captcha" in m or "verification" in m:
            return "verification"
        if "timed out" in m or "timeout" in m:
            return "timeout"
        if "missing_" in m or "api_error" in m:
            return "api_error"
        if "no_results" in m:
            return "no_results"
        return "api_error"

    def _summarize_titles(self, items: List[Dict[str, Any]]) -> str:
        titles = [str(it.get("title", "")).strip() for it in items if str(it.get("title", "")).strip()]
        if not titles:
            return "No results found."
        return "Top findings: " + "; ".join(titles[:5])

    def _report_error(self, message: str) -> None:
        self.report_result(self.parent_pid, {"status": "error", "error": message}, self.entry_trace_id)


if __name__ == "__main__":
    WebSearchSkill().start()
