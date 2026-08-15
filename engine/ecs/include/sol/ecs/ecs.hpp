#pragma once

// Umbrella header for sol::ecs. Storage design: sparse-set per component
// type (docs/decisions/001-ecs-storage-model.md).

#include "sol/ecs/command_buffer.hpp"
#include "sol/ecs/entity.hpp"
#include "sol/ecs/registry.hpp"
#include "sol/ecs/snapshot.hpp"
#include "sol/ecs/sparse_set.hpp"
#include "sol/ecs/view.hpp"
