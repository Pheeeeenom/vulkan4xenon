"""Breaks milestone 28's checks in a copy of the layer, a set at a time: breaks.py <copy> <set>, or breaks.py --list.

Each break is a small source change that should fail the vulkan4xenon_test checks named beside it, and no others;
builds.sh builds each set into a title of its own. Sets whose breaks would fail the same checks are kept apart. Xenia
sets every query's bit, so only the console fails the clears check.
- a: a clear inside a query counted toward it, its id left running; precise queries taken.
- b: a query's last id's bit read alone, not every id's it took; queries begun outside a render pass taken.
- c: resets leaving occlusion queries as they were, so a query written before stays available.
- d: a query's pool not marked used by its submission, so a wait for its results doesn't wait.
"""
import pathlib
import sys

CLEARS = "occlusion: vkCmdClearAttachments inside a query doesn't count, and a draw either side of it does"
PRECISE = "occlusion: occlusionQueryPrecise and inheritedQueries aren't reported, and a precise query is refused"
OUTSIDE = "occlusion: a query begun outside a render pass is refused"
COPY = ("occlusion: vkCmdCopyQueryPoolResults copies results at 64 bits with availability and at 32, an unwritten "
        "query unavailable and its value untouched")
GET = ("occlusion: vkGetQueryPoolResults reads results with availability at 32 bits, an unwritten query unavailable "
       "and VK_NOT_READY")
MANY = "occlusion: 80 queries in one submission, quads nonzero and no draws 0, which a wait for results waits for"

# Each set's breaks: the file, the text it changes, what it becomes, and the checks it should fail.
SETS = {
    "a": [
        ("src/queue.c", "const bool pauses_query = replay->query_address && replay->visibility_id + 1 < "
         "VISIBILITY_WORD_IDS;", "const bool pauses_query = false;", [CLEARS]),
        ("src/command_buffer.c", "if (flags & VK_QUERY_CONTROL_PRECISE_BIT) {", "if (flags & 0) {", [PRECISE]),
    ],
    "b": [
        ("src/query.c", "(slot[0] & ((2u << last) - 1)) != 0", "(slot[0] >> last & 1) != 0", [CLEARS]),
        ("src/command_buffer.c", "if (!commandBuffer->render_pass && !commandBuffer->continues_pass) {",
         "if (commandBuffer->secondary && !commandBuffer->continues_pass) {", [OUTSIDE]),
    ],
    "c": [
        ("src/transfer.c", "slot[word] = V4X_QUERY_UNWRITTEN;",
         "slot[word] = pool->occlusion ? slot[word] : V4X_QUERY_UNWRITTEN;", [COPY, GET]),
    ],
    "d": [
        ("src/queue.c", "xsdk_gpu_begin_visibility(0);\n\tpool->used_serial = replay->serial;\n",
         "xsdk_gpu_begin_visibility(0);\n", [MANY]),
    ],
}


def apply(root, name):
    texts = {}
    for path, old, new, checks in SETS[name]:
        file = root / path
        text = texts.get(file, file.read_text())
        assert text.count(old) == 1, (path, old)
        texts[file] = text.replace(old, new)
        print(f"{name}: {path}: should fail {'; '.join(checks)}")
    for file, text in texts.items():
        file.write_text(text)


if sys.argv[1] == "--list":
    print(" ".join(SETS))
else:
    apply(pathlib.Path(sys.argv[1]), sys.argv[2])
