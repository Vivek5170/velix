---
name: git-expert
description: Git workflow guidance, commit best practices, branch strategies, and conflict resolution techniques.
author: Velix Team
version: 1.0.0
visibility: public
tags:
  - git
  - vcs
  - workflow
---

# Git Expert Skill

## Context

Git is a distributed version control system that tracks changes across collaborative development projects. Understanding Git workflows, branching strategies, and best practices helps teams coordinate changes, maintain code quality, and manage releases effectively.

## Instructions

When users ask about Git workflows, commit messages, branching strategies, or conflict resolution:

1. **Commit Messages**: Follow conventional commits format (type: scope: message)
   - `feat:` for new features
   - `fix:` for bug fixes
   - `docs:` for documentation
   - `refactor:` for code restructuring
   - `test:` for test additions

2. **Branching**: Recommend appropriate strategies
   - `main`: production-ready code
   - `develop`: integration branch
   - `feature/*`: feature branches from develop
   - `hotfix/*`: production fixes from main

3. **Conflict Resolution**: Guide through merge conflicts
   - Identify conflicting sections
   - Preserve intent from both sides when possible
   - Test thoroughly after resolution
   - Document complex decisions

4. **Best Practices**
   - Keep commits small and focused
   - Rebase before pushing (keep history clean)
   - Use tags for releases
   - Review all PRs thoroughly

## Examples

**Good commit message:**
```
feat: add user authentication middleware

Implement JWT-based authentication for API endpoints.
Supports token refresh and expiration handling.
```

**Branch workflow:**
```bash
# Create feature branch
git checkout -b feature/user-auth

# Make commits
git commit -m "feat: add JWT validation"

# Push and create PR
git push -u origin feature/user-auth

# After review and approval
git checkout develop
git merge feature/user-auth
```

## Related Tools

- **terminal**: Execute git commands
- **session_search**: Search git history and documentation
- **web_search**: Find Git best practices and solutions
