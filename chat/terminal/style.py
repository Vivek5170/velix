"""
style.py — Velix Terminal visual constants.

All ANSI escape codes, color strings, kawaii face arrays,
tool emojis, and the Velix ASCII logo live here.
Nothing in this file has side-effects; just import and use.
"""

# =============================================================================
# ANSI escape codes
# =============================================================================

RST        = "\033[0m"
BOLD       = "\033[1m"
DIM        = "\033[2m"
ITALIC     = "\033[3m"

BOLD_CYAN  = "\033[1;36m"
DIM_CYAN   = "\033[2;36m"
CYAN       = "\033[36m"
BOLD_RED   = "\033[1;31m"
BOLD_BLUE  = "\033[1;34m"
BOLD_WHITE = "\033[1;37m"
WHITE      = "\033[0;37m"
YELLOW     = "\033[0;33m"
BOLD_YELLOW= "\033[1;33m"
DIM_WHITE  = "\033[2;37m"
GREEN      = "\033[0;32m"
BOLD_GREEN = "\033[1;32m"

# Gold — matches the Velix brand (#FFD700 true-color)
GOLD       = "\033[1;38;2;255;215;0m"
GOLD_DIM   = "\033[2;38;2;255;215;0m"

# Bronze (#CD7F32 true-color) — used for box borders / rules
BRONZE     = "\033[38;2;205;127;50m"
BRONZE_BOLD= "\033[1;38;2;205;127;50m"

# Amber (#FFBF00 true-color) — secondary highlight
AMBER      = "\033[38;2;255;191;0m"

# Response-text colour (#FFF8DC cornsilk)
CORNSILK   = "\033[38;2;255;248;220m"

# =============================================================================
# Velix ASCII logo (plain text for Rich rendering)
# =============================================================================

VELIX_LOGO_TEXT = (
    "██╗   ██╗███████╗██╗     ██╗██╗  ██╗\n"
    "██║   ██║██╔════╝██║     ██║╚██╗██╔╝\n"
    "██║   ██║█████╗  ██║     ██║ ╚███╔╝ \n"
    "╚██╗ ██╔╝██╔══╝  ██║     ██║ ██╔██╗ \n"
    " ╚████╔╝ ███████╗███████╗██║██╔╝ ██╗\n"
    "  ╚═══╝  ╚══════╝╚══════╝╚═╝╚═╝  ╚═╝"
)
VELIX_TAGLINE_TEXT = "The Agentic Operating System"

# =============================================================================
# Kawaii face arrays
# =============================================================================

# General waiting / thinking faces
KAWAII_WAITING = [
    "(｡◕‿◕｡)", "(◕‿◕✿)", "٩(◕‿◕｡)۶", "(✿◠‿◠)", "( ˘▽˘)っ",
    "♪(´ε` )", "(◕ᴗ◕✿)", "ヾ(＾∇＾)", "(≧◡≦)", "(★ω★)",
]

KAWAII_THINKING = [
    "(｡•́︿•̀｡)", "(◔_◔)", "(¬‿¬)", "( •_•)>⌐■-■", "(⌐■_■)",
    "(´･_･`)", "◉_◉", "(°ロ°)", "( ˘⌣˘)♡", "ヾ(>∀<☆)☆",
    "٩(๑❛ᴗ❛๑)۶", "(⊙_⊙)", "(¬_¬)", "( ͡° ͜ʖ ͡°)", "ಠ_ಠ",
]

KAWAII_WORKING = [
    "ヽ(>∀<☆)ノ", "(ノ°∀°)ノ", "٩(^ᴗ^)۶", "ヾ(⌐■_■)ノ♪", "(•̀ᴗ•́)و",
    "┗(＾0＾)┓", "(｀・ω・´)", "＼(￣▽￣)／", "(ง •̀_•́)ง", "ヽ(´▽`)/",
]

# Faces used as periodic spinner heartbeat lines
KAWAII_SPINNER_FACES = [
    "(｡◕‿◕｡)", "(◕‿◕✿)", "٩(◕‿◕｡)۶", "(✿◠‿◠)", "( ˘▽˘)っ",
    "♪(´ε` )", "(◕ᴗ◕✿)", "ヾ(＾∇＾)", "(≧◡≦)", "(★ω★)",
    "(｡•́︿•̀｡)", "(◔_◔)", "(¬‿¬)", "(ง •̀_•́)ง", "(ﾉ◕ヮ◕)ﾉ*:･ﾟ✧",
]

# Braille spinner frames (used as rotating prefix in heartbeat)
BRAILLE_FRAMES = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"]

# =============================================================================
# Tool emoji map
# =============================================================================

TOOL_EMOJIS: dict[str, str] = {
    "terminal":          "💻",
    "web_search":        "🔍",
    "read_file":         "📖",
    "write_file":        "✍️",
    "patch":             "🔧",
    "browser_navigate":  "🌐",
    "browser_snapshot":  "📸",
    "browser_click":     "👆",
    "browser_type":      "⌨️",
    "skill_view":        "📚",
    "skills_list":       "📚",
    "image_generate":    "🎨",
    "cronjob":           "⏰",
    "vision_analyze":    "👁️",
    "text_to_speech":    "🔊",
    "delegate_task":     "🔀",
    "execute_code":      "🐍",
    "memory":            "🧠",
    "session_search":    "🔍",
    "todo":              "📋",
    "send_message":      "📨",
    "web_extract":       "📄",
}

def get_tool_emoji(tool_name: str, default: str = "⚡") -> str:
    return TOOL_EMOJIS.get(tool_name, default)