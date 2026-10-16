"""Breaks milestone 24's checks in a copy of the layer that builds.sh made: breaks.py <set> <copy>.

Step 1, instancing: a breaks each row's draw and the feature on its own, so each of those six checks fails alone:
vkCmdDraw dropping all but one instance from instance 0, vkCmdDrawIndexed recording a first instance of 0,
vkCmdDrawIndirect reading none, vkCmdDrawIndexedIndirect reading its vertex offset as it, streams at the instance's rate
moved only where the shader reads gl_InstanceIndex, and drawIndirectFirstInstance unreported. b fetches the instance's
bindings at their stride, failing all five rows. c never writes the index, failing the four rows that read it and not
the fifth.

Step 2, strips: d steps a triangle strip's pieces an odd number of vertices on, failing both triangle strip checks; a
line strip's without the vertex they share, failing the line strip's; and takes long fans, failing the fan's. e has an
indexed draw's pieces share nothing, failing the indexed triangle strip's check alone. f draws a strip's first piece
alone, failing all three strips' checks and no list's.
"""
import pathlib
import sys

SETS = {
    "a": [
        ("src/command_buffer.c", "command->draw.instance_count = instanceCount;",
         "command->draw.instance_count = firstInstance ? instanceCount : 1;"),
        ("src/command_buffer.c", "command->draw_indexed.first_instance = firstInstance;",
         "command->draw_indexed.first_instance = firstInstance * 0;"),
        ("src/queue.c", "draw(replay, parameters[0], parameters[2], parameters[1], parameters[3]);",
         "draw(replay, parameters[0], parameters[2], parameters[1], 0);"),
        ("src/queue.c", "(int32_t)parameters[3], parameters[1], parameters[4]);",
         "(int32_t)parameters[3], parameters[1], parameters[3]);"),
        ("src/queue.c", "source->per_instance ? instance * source->stride_bytes",
         "source->per_instance && pipeline->reads_instance_index ? instance * source->stride_bytes"),
        ("src/physical_device.c", ".drawIndirectFirstInstance = VK_TRUE, ", ""),
    ],
    "b": [
        ("src/pipeline.c", "strides[i] = per_instance ? 0 : binding->stride;", "strides[i] = binding->stride;"),
    ],
    "c": [
        ("src/queue.c", "\txsdk_gpu_set_vertex_constants(pipeline->instance_index_constant, values, 1);\n",
         "\t(void)values;\n"),
    ],
    "d": [
        ("src/pipeline_state.c", "pipeline->piece_step = V4X_MAX_DRAW_VERTICES - 3;",
         "pipeline->piece_step = V4X_MAX_DRAW_VERTICES - 2;"),
        ("src/pipeline_state.c", "pipeline->piece_step = V4X_MAX_DRAW_VERTICES - 1;",
         "pipeline->piece_step = V4X_MAX_DRAW_VERTICES;"),
        ("src/command_buffer.c", "!buffer->pipeline || buffer->pipeline->piece_step)",
         "!buffer->pipeline || buffer->pipeline->piece_vertices)"),
    ],
    "e": [
        ("src/queue.c", "replay->index_address + (first_index + start) * index_bytes;",
         "replay->index_address + (first_index + start / pipeline->piece_step * pipeline->piece_vertices) * "
         "index_bytes;"),
    ],
    "f": [
        ("src/queue.c", "pipeline->piece_vertices);\n\t\tif (left <= pipeline->piece_vertices)\n",
         "pipeline->piece_vertices);\n\t\tif (left <= pipeline->piece_vertices || "
         "pipeline->piece_step != pipeline->piece_vertices)\n"),
        ("src/queue.c", "replay->index_32_bit);\n\t\tif (left <= pipeline->piece_vertices)\n",
         "replay->index_32_bit);\n\t\tif (left <= pipeline->piece_vertices || "
         "pipeline->piece_step != pipeline->piece_vertices)\n"),
    ],
}


def apply(name, root):
    texts = {}
    for path, old, new in SETS[name]:
        file = root / path
        text = texts.get(file) or file.read_text()
        assert text.count(old) == 1, (path, old)
        texts[file] = text.replace(old, new)
    for file, text in texts.items():
        file.write_text(text)
    print(f"set {name}: {len(SETS[name])} breaks in {len(texts)} files")


apply(sys.argv[1], pathlib.Path(sys.argv[2]))
