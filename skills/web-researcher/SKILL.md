---
name: web-researcher
description: Web research strategies, source evaluation, fact verification, and information synthesis from multiple sources.
author: Velix Team
version: 1.0.0
visibility: public
tags:
  - research
  - web-search
  - verification
  - information-synthesis
---

# Web Researcher Skill

## Context

Effective web research requires strategic query formulation, source evaluation, cross-verification, and synthesis of information from multiple perspectives. Quality research avoids misinformation and provides accurate, well-sourced answers.

## Instructions

When conducting web research:

1. **Query Strategy**
   - Start broad, then refine: "AI safety" → "AI alignment research 2024"
   - Use specific terms for technical topics
   - Include date constraints for time-sensitive topics: "machine learning 2024"
   - Use quotes for exact phrases: `"attention is all you need"`
   - Exclude irrelevant terms with minus: `-spam -scam`

2. **Source Evaluation**
   - **Primary sources**: Research papers, official documentation, government data
   - **Secondary sources**: Reviews, analyses, educational content from experts
   - **Avoid**: Anonymous blogs, unsourced opinion pieces, outdated pages
   - Check publication date and author credentials
   - Look for citations and references (stronger sources cite their claims)

3. **Fact Verification**
   - Cross-check facts across multiple independent sources
   - Check both supporting and contrasting viewpoints
   - Look for consensus among domain experts
   - Be skeptical of extraordinary claims without multiple sources
   - Verify statistics at their original source, not just cited sources

4. **Information Synthesis**
   - Extract key points from each source
   - Identify consensus and disagreements
   - Note when sources disagree and explain why
   - Synthesize into coherent summary with proper attributions
   - Maintain nuance and uncertainty where appropriate

## Examples

**Good research workflow:**

1. Query: "GraphQL vs REST APIs advantages disadvantages"
2. Sources: Retrieve 5-10 authoritative sources
   - Official GraphQL documentation
   - REST API best practices from industry leaders
   - Comparative analyses from respected tech publications
   - Real-world case studies from companies using both

3. Analysis:
   - GraphQL advantages: Single endpoint, request exactly what you need, no over/under-fetching
   - GraphQL disadvantages: Steeper learning curve, caching complexity, query complexity limits
   - REST advantages: Simple, cacheable, mature ecosystem
   - REST disadvantages: Over/under-fetching, multiple endpoints

4. Synthesis: "GraphQL excels for complex, interconnected data; REST for simpler, cacheable APIs. Many companies use both."

## Troubleshooting

- **"I'm finding contradictory information"**: Normal—summarize positions with sources
- **"The information seems outdated"**: Add date to search, prioritize recent publications
- **"All results are promotional"**: Add `-review -ad -sponsored`, refine query terms
- **"Too many results"**: Use more specific search terms, site-specific search (site:github.com)

## Related Tools

- **web_search**: Execute web searches and retrieve sources
- **terminal**: Download papers, extract text from PDFs
- **session_search**: Track research notes and cross-reference findings
