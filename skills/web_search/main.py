#!/usr/bin/env python3
import json
import urllib.parse
import urllib.request
import urllib.error
from typing import Any, Dict, List

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

        requested_sources = self.params.get("sources", ["wikipedia", "crossref"])
        if not isinstance(requested_sources, list):
            requested_sources = ["wikipedia", "crossref"]
        sources = {str(s).strip().lower() for s in requested_sources}
        if not sources:
            sources = {"wikipedia", "crossref"}

        warnings: List[str] = []

        wikipedia: List[Dict[str, Any]] = []
        crossref: List[Dict[str, Any]] = []

        if "wikipedia" in sources:
            try:
                wikipedia = self._search_wikipedia(query, max_results, timeout_sec)
            except Exception as exc:
                warnings.append(f"wikipedia_error: {exc}")

        if "crossref" in sources:
            try:
                crossref = self._search_crossref(query, max_results, timeout_sec)
            except Exception as exc:
                warnings.append(f"crossref_error: {exc}")

        all_items = wikipedia + crossref
        result = {
            "status": "success",
            "query": query,
            "results": all_items,
            "summary": self._summarize_titles(all_items),
            "sources_used": sorted([s for s in ["wikipedia", "crossref"] if s in sources]),
            "note": "Results fetched from official public APIs; no anti-bot bypass techniques are used.",
            "warnings": warnings,
        }

        self.report_result(self.parent_pid, result, self.entry_trace_id)

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
        return out

    def _search_crossref(self, query: str, limit: int, timeout_sec: int) -> List[Dict[str, Any]]:
        params = {
            "query.bibliographic": query,
            "rows": str(limit),
            "sort": "relevance",
            "order": "desc",
        }
        url = "https://api.crossref.org/works?" + urllib.parse.urlencode(params)
        payload = self._get_json(url, timeout_sec)
        out: List[Dict[str, Any]] = []
        for item in payload.get("message", {}).get("items", []):
            title_list = item.get("title", [])
            title = str(title_list[0] if title_list else "").strip()
            doi = str(item.get("DOI", "")).strip()
            link = "https://doi.org/" + doi if doi else str(item.get("URL", "")).strip()
            container = item.get("container-title", [])
            venue = str(container[0] if container else "")
            out.append(
                {
                    "source": "crossref",
                    "title": title,
                    "url": link,
                    "snippet": venue,
                }
            )
        return out

    def _get_json(self, url: str, timeout_sec: int) -> Dict[str, Any]:
        req = urllib.request.Request(
            url,
            headers={
                "User-Agent": "VelixWebSearchSkill/1.0 (+https://example.invalid)",
                "Accept": "application/json",
            },
            method="GET",
        )
        with urllib.request.urlopen(req, timeout=timeout_sec) as resp:
            raw = resp.read().decode("utf-8", errors="replace")
            return json.loads(raw)

    def _summarize_titles(self, items: List[Dict[str, Any]]) -> str:
        titles = [str(it.get("title", "")).strip() for it in items if str(it.get("title", "")).strip()]
        if not titles:
            return "No results found."
        return "Top findings: " + "; ".join(titles[:5])

    def _report_error(self, message: str) -> None:
        self.report_result(self.parent_pid, {"status": "error", "error": message}, self.entry_trace_id)


if __name__ == "__main__":
    WebSearchSkill().start()
