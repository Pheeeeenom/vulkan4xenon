"""Breaks each of milestone 19 step 1's checks in the layer, or restores it: step1_breaks.py apply|restore."""
import pathlib
import sys

ROOT = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else "/mnt/z/vulkan4xenon")
BACKUP = pathlib.Path(__file__).with_name("step1_backup")

BREAKS = [
    ("src/proc_addr.c", "\tENTRY(vkResetEvent, SCOPE_DEVICE),\n", ""),
    ("src/pipeline.c", "header[0] = CACHE_HEADER_BYTES;", "header[3] = CACHE_HEADER_BYTES;"),
    ("src/pipeline.c", "\t\t*pDataSize = 0;\n\t\treturn VK_INCOMPLETE;", "\t\tmemset(pData, 0, *pDataSize);\n\t\treturn VK_INCOMPLETE;"),
    ("src/pipeline.c", "(void)pSrcCaches;\n\treturn VK_SUCCESS;", "(void)pSrcCaches;\n\treturn VK_INCOMPLETE;"),
    ("src/sync.c", "\t*pEvent = v4x_handle(event);", "\tevent->set = true;\n\t*pEvent = v4x_handle(event);"),
    ("src/render_pass.c", "(VkExtent2D){ 1, 1 }", "(VkExtent2D){ 80, 16 }"),
    ("src/memory.c", "((const DeviceMemory *)v4x_object(memory))->size_bytes;", "0;\n\t(void)memory;"),
    ("src/image.c", "*pSparseMemoryRequirementCount = 0;", "*pSparseMemoryRequirementCount = 1;"),
    ("src/queue.c", 'return v4x_host_unsupported("sparse binding");', "return VK_SUCCESS;"),
    ("src/command_buffer.c", '(void)groupCountZ;\n\tcommandBuffer->result = v4x_host_unsupported("compute dispatches");',
     "(void)groupCountZ;\n\t(void)commandBuffer;"),
    ("src/pipeline_state.c", "else if (dynamic->pDynamicStates[i] != VK_DYNAMIC_STATE_LINE_WIDTH)", "else"),
]


def apply():
    BACKUP.mkdir(exist_ok=True)
    texts = {}
    for path, old, new in BREAKS:
        if path not in texts:
            texts[path] = (ROOT / path).read_text()
            (BACKUP / path.replace("/", "_")).write_text(texts[path])
        assert texts[path].count(old) == 1, (path, old)
        texts[path] = texts[path].replace(old, new)
    for path, text in texts.items():
        (ROOT / path).write_text(text)
    print(f"{len(BREAKS)} breaks in {len(texts)} files")


def restore():
    for path in {path for path, _, _ in BREAKS}:
        (ROOT / path).write_text((BACKUP / path.replace("/", "_")).read_text())
    print("restored")


{"apply": apply, "restore": restore}[sys.argv[1]]()
