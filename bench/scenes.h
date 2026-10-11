#ifndef V4X_BENCH_SCENES_H
#define V4X_BENCH_SCENES_H

#include "bench.h"

typedef enum {
	// Many draws, no state changes.
	SCENE_DRAWS,
	// Many draws, with state changes between them.
	SCENE_STATES,
	// Many draws, each one's matrix and tint from dynamic uniform buffers.
	SCENE_UNIFORMS,
	// Sprites moved and written out every frame.
	SCENE_STREAM,
	// Full-screen layers, bound by the GPU.
	SCENE_FILL,
} SceneKind;

typedef struct {
	const char *name;
	SceneKind kind;
	int count;
} SceneDef;

void scene_setup(const SceneDef *scene);
// Steps by frame number, so both backends get the same data.
void scene_update(const SceneDef *scene, int frame);
void scene_render(const SceneDef *scene, const BenchBackend *backend);

#endif
