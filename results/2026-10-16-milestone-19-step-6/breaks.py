"""Breaks milestone 19 step 6's checks, or restores them: step6_breaks.py apply a|b|c, or restore.

Set a breaks nine checks, one break each: vkCmdWriteTimestamp's name dropped, 32 valid bits, statistics pools made,
vkCmdResetEvent setting, vkCmdWaitEvents waiting on nothing, time stamps' high word dropped, every time stamp taken at the
top of the pipe, copies' 64-bit results written as 32, and resets marking nothing. b sets the period to 1 ns, which the
period check and the 50 ms check both read. c fails vkCreateEvent, and with it the scene.
"""
import pathlib
import sys

BACKUP = pathlib.Path(__file__).with_name("step6_backup")
LAYER = pathlib.Path("/mnt/z/vulkan4xenon")

SETS = {
    "a": [
        (LAYER / "src/proc_addr.c", "\tENTRY(vkCmdWriteTimestamp, SCOPE_DEVICE),\n", ""),
        (LAYER / "src/physical_device.c", ".timestampValidBits = TIMESTAMP_BITS,", ".timestampValidBits = 32,"),
        (LAYER / "src/query.c", "if (pCreateInfo->queryType != VK_QUERY_TYPE_TIMESTAMP)",
         "if (pCreateInfo->queryType != VK_QUERY_TYPE_TIMESTAMP && pCreateInfo->queryType != VK_QUERY_TYPE_PIPELINE_STATISTICS)"),
        (LAYER / "src/command_buffer.c", "record_event(commandBuffer, event, 0);", "record_event(commandBuffer, event, 1);"),
        (LAYER / "src/queue.c", "i < command->wait_events.count; i++) {", "i < command->wait_events.count * 0; i++) {"),
        (LAYER / "src/query.c", "*value = xsdk_gpu_clock_value(slot);", "*value = (uint32_t)xsdk_gpu_clock_value(slot);"),
        (LAYER / "src/command_buffer.c", "command->timestamp.after_work = pipelineStage != VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;",
         "command->timestamp.after_work = pipelineStage == 0;"),
        (LAYER / "src/transfer.c", "\tif (wide) {\n\t\tout[2 * index]", "\tif (wide && !wide) {\n\t\tout[2 * index]"),
        (LAYER / "src/transfer.c", "i < command->queries.count; i++) {", "i < command->queries.count * 0; i++) {"),
    ],
    "b": [
        (LAYER / "src/physical_device.c", "timestamp_period_ns = 1000.0f / 498.9f;", "timestamp_period_ns = 1.0f;"),
    ],
    "c": [
        (LAYER / "src/sync.c", "\tevent->word = xsdk_gpu_alloc(sizeof *event->word);\n",
         "\tevent->word = NULL;\n"),
    ],
}


def backup_name(path):
    return str(path).replace("/", "_")


def apply(name):
    BACKUP.mkdir(exist_ok=True)
    texts = {}
    for path, old, new in SETS[name]:
        if path not in texts:
            texts[path] = path.read_text()
            (BACKUP / backup_name(path)).write_text(texts[path])
        assert texts[path].count(old) == 1, (path, old)
        texts[path] = texts[path].replace(old, new)
    for path, text in texts.items():
        path.write_text(text)
    (BACKUP / "applied").write_text("\n".join(str(p) for p in texts))
    print(f"set {name}: {len(SETS[name])} breaks in {len(texts)} files")


def restore():
    for path in (BACKUP / "applied").read_text().split("\n"):
        pathlib.Path(path).write_text((BACKUP / backup_name(path)).read_text())
    (BACKUP / "applied").unlink()
    print("restored")


if sys.argv[1] == "apply":
    apply(sys.argv[2])
else:
    restore()
