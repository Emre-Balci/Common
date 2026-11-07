#!/usr/bin/env bash
# Runs a coding agent on a task in the task's own git worktree, so tasks run in parallel and move
# between agents. The task's state is in its branch `task/<name>` and its handoff file
# `.agents/handoffs/<name>.md`, not in any agent's session (see AGENTS.md).
#
#   scripts/agent-task.sh [-p] <agent> <task> [instructions...]   start or continue a task
#   scripts/agent-task.sh list                                    list task worktrees
#   scripts/agent-task.sh remove <task>                           remove a task's worktree (keeps the branch)
#
# Agents: claude, opencode, codex, hermes. -p runs the agent once without its interactive UI and
# returns when it is done (claude -p, opencode run, codex exec, hermes chat --oneshot), for scripts
# and for one agent delegating to another.
#
# Tiers, which choose the agent and the model (see "Which agent" in AGENTS.md):
#   heavy   Claude Code with Opus (your Claude subscription): design, hard bugs, RT/concurrency code
#   small   OpenCode with the local llama.cpp server: mechanical edits, docs, simple tests
#
# Environment:
#   AGENT_HEAVY_MODEL  model of the heavy tier (default: opus)
#   AGENT_SMALL_MODEL  model of the small tier (default: llamacpp/local, see ~/.config/opencode)
#   AGENT_SMALL_TIMEOUT  seconds a one-shot small run may take (default 900)
#   LLAMA_URL          base URL of llama-server's OpenAI API, used by OpenCode's llamacpp provider
#                      (default: port 8080 on the Windows host, which WSL reaches at its gateway)
#   AGENT_WORKTREES  where worktrees go (default ~/agent-worktrees, on the Linux file system, because
#                    building on /mnt/<drive> is slow). Windows git does not see these paths:
#                    do not run `git worktree prune` from Windows while tasks are open.
#   AGENT_BASE       commit a new task branch starts from (default: HEAD of the main worktree)
set -euo pipefail

usage() {
    sed -n '2,/^set -e/p' "$0" | grep '^#' | sed 's/^# \{0,1\}//' >&2
    exit 2
}

die() {
    printf 'agent-task: %s\n' "$*" >&2
    exit 1
}

common_dir="$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null)" ||
    die "not inside a git repository"
main_root="$(dirname "$common_dir")"
repo_name="$(basename "$main_root")"
worktree_root="${AGENT_WORKTREES:-$HOME/agent-worktrees}/$repo_name"

check_task() {
    [[ "$1" =~ ^[a-z0-9][a-z0-9-]{0,47}$ ]] ||
        die "task name '$1' must be lower-case letters, digits and '-' (at most 48)"
}

cmd="${1:-}"
case "$cmd" in
    "" | -h | --help)
        usage
        ;;
    list)
        git -C "$main_root" worktree list | grep -F '[task/' || echo "no task worktrees"
        exit 0
        ;;
    remove)
        [[ $# -eq 2 ]] || usage
        check_task "$2"
        # Refuses a worktree with uncommitted changes: commit or discard them first.
        git -C "$main_root" worktree remove "$worktree_root/$2"
        echo "removed the worktree; branch task/$2 is kept (git branch -d task/$2 once it is merged)"
        exit 0
        ;;
esac

once=false
if [[ "$cmd" == "-p" ]]; then
    once=true
    shift
fi
[[ $# -ge 2 ]] || usage
agent="$1"
task="$2"
shift 2
instructions="$*"
check_task "$task"

model=""
tier=""
case "$agent" in
    heavy)
        agent=claude
        tier=heavy
        model="${AGENT_HEAVY_MODEL:-opus}"
        ;;
    small)
        agent=opencode
        tier=small
        model="${AGENT_SMALL_MODEL:-llamacpp/local}"
        ;;
    claude | opencode | codex | hermes) ;;
    *) die "unknown agent '$agent' (heavy, small, claude, opencode, codex, hermes)" ;;
esac
command -v "$agent" >/dev/null || die "'$agent' is not on PATH"

if [[ "$agent" == opencode && "$model" == llamacpp/* ]]; then
    # NAT-mode WSL cannot reach the Windows host at localhost; its default gateway is the host.
    : "${LLAMA_URL:=http://$(ip route | awk '/^default/{print $3; exit}'):8080/v1}"
    export LLAMA_URL
    curl -fsS --max-time 3 "${LLAMA_URL%/v1}/health" >/dev/null 2>&1 ||
        die "no llama-server answers at $LLAMA_URL: start it on Windows with --host 0.0.0.0 (see AGENTS.md)"
fi

dir="$worktree_root/$task"
branch="task/$task"
handoff=".agents/handoffs/$task.md"
if [[ ! -f "$dir/$handoff" && -z "$instructions" ]]; then
    die "new task '$task': give instructions (they become its goal)"
fi
if [[ ! -d "$dir" ]]; then
    mkdir -p "$worktree_root"
    if git -C "$main_root" show-ref --verify --quiet "refs/heads/$branch"; then
        git -C "$main_root" worktree add "$dir" "$branch"
    else
        git -C "$main_root" worktree add -b "$branch" "$dir" "${AGENT_BASE:-HEAD}"
    fi
fi

if [[ ! -f "$dir/$handoff" ]]; then
    template="$dir/.agents/handoff-template.md"
    [[ -f "$template" ]] || die "$template is missing: start the task from a branch that has it"
    {
        printf '# Handoff: %s\n\n## Goal\n%s\n\n' "$task" "$instructions"
        printf '## State (%s, created by agent-task.sh)\n- Branch `%s`; nothing done yet.\n\n' \
            "$(date +%F)" "$branch"
        printf '## Findings\n\n## Not verified\n\n## Next steps\n1. Start on the goal.\n'
    } >"$dir/$handoff"
fi

if [[ "$tier" == small ]]; then
    # The small tier's window is 16k tokens: it gets a short rules file instead of AGENTS.md, and the
    # script, not the model, writes the handoff's run log afterwards (see small_finish).
    prompt="You are working on task '$task' in a git worktree on branch $branch. Read .agents/small-rules.md \
and $handoff (its Goal is your task), then do the task. \
${instructions:+Instructions for this session: $instructions } \
When it is done, commit your changes on $branch with a short message. Do not edit $handoff. \
Do not push or merge."
else
    prompt="You are working on task '$task' in a git worktree on branch $branch. First read AGENTS.md \
and $handoff, which holds the goal and the state the previous agent left. \
${instructions:+Instructions for this session: $instructions } \
Before you stop, update $handoff (State, Not verified, Next steps) so another agent can continue, \
and commit your work and the handoff on $branch. Do not push or merge."
fi

# Runs after a small-tier one-shot run: records what happened in the handoff, whatever the model did.
small_finish() {
    local status="$1" started="$2"
    {
        printf '\n## Last run (%s, small tier, %s)\n' "$(date '+%F %T')" "$model"
        printf -- '- Agent exit status %s after %s s.\n' "$status" "$((SECONDS - started))"
        printf -- '- Commits on this branch since its start:\n'
        # The branch's first reflog entry is the commit it was created from.
        git log --format='  - %h %s' "$(git reflog show --format=%H "$branch" | tail -n 1)..HEAD" 2>/dev/null || true
        printf -- '- Uncommitted changes:\n'
        git status --short | sed 's/^/  - /'
        printf -- '- Not reviewed: have a `heavy` task check this branch before it is merged.\n'
    } >>"$handoff"
    git add -- "$handoff"
    git commit -q -m "Handoff: record the small-tier run of $task" -- "$handoff" || true
}

cd "$dir"
echo "agent-task: $agent${model:+ ($model)} on $branch in $dir" >&2
model_args=()
[[ -z "$model" ]] || model_args=(--model "$model")
if [[ "$once" == true ]]; then
    case "$agent" in
        claude)
            # Headless: edits and the build tools are allowed, anything else is refused.
            exec claude -p "${model_args[@]}" --permission-mode acceptEdits \
                --allowedTools "Bash(git:*)" "Bash(cmake:*)" "Bash(ctest:*)" "Bash(ninja:*)" -- "$prompt"
            ;;
        opencode)
            if [[ "$tier" == small ]]; then
                started=$SECONDS
                status=0
                # Closed stdin: opencode run waits on one that is not a terminal. The time limit
                # keeps a model that loops from running for ever (AGENT_SMALL_TIMEOUT, seconds).
                timeout "${AGENT_SMALL_TIMEOUT:-900}" opencode run "${model_args[@]}" "$prompt" </dev/null ||
                    status=$?
                small_finish "$status" "$started"
                exit "$status"
            fi
            exec opencode run "${model_args[@]}" "$prompt" </dev/null
            ;;
        codex) exec codex exec "$prompt" ;;
        hermes) exec hermes chat --oneshot -q "$prompt" ;;
    esac
else
    case "$agent" in
        claude) exec claude "${model_args[@]}" "$prompt" ;;
        opencode) exec opencode "${model_args[@]}" --prompt "$prompt" . ;;
        codex) exec codex "$prompt" ;;
        hermes) exec hermes chat -q "$prompt" ;;
    esac
fi
