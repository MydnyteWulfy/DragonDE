// wl_compositor / wl_surface / wl_region / wl_subsurface / wl_shm behavior.

#include "ServerFixture.h"

#include <drm_fourcc.h>

using namespace dragon;
using dragon::test::ServerFixture;

namespace {
using SurfaceTest = ServerFixture;
}

TEST_F(SurfaceTest, ShmBufferBecomesCurrentOnCommit)
{
    auto* s = makeSurface();
    wl_surface_attach(s, makeBuffer(64, 32), 0, 0);
    wl_surface_damage_buffer(s, 0, 0, 64, 32);
    ASSERT_TRUE(roundtrip());
    ASSERT_EQ(surfaces.size(), 1u);
    EXPECT_EQ(surfaces[0]->buffer(), nullptr) << "attach is pending until commit";

    wl_surface_commit(s);
    ASSERT_TRUE(roundtrip());

    auto* buffer = surfaces[0]->buffer();
    ASSERT_NE(buffer, nullptr);
    EXPECT_EQ(buffer->kind(), ClientBuffer::Kind::Shm);
    EXPECT_EQ(buffer->size(), (Size{64, 32}));
    EXPECT_EQ(buffer->format(), DRM_FORMAT_XRGB8888);
    EXPECT_EQ(surfaces[0]->size(), (Size{64, 32}));
    EXPECT_TRUE(surfaces[0]->state().bufferDamage.contains({63, 31}));
}

TEST_F(SurfaceTest, ScaleAndTransformAffectSurfaceSize)
{
    auto* s = makeSurface();
    wl_surface_attach(s, makeBuffer(200, 100), 0, 0);
    wl_surface_set_buffer_scale(s, 2);
    wl_surface_set_buffer_transform(s, WL_OUTPUT_TRANSFORM_90);
    wl_surface_commit(s);
    ASSERT_TRUE(roundtrip());

    EXPECT_EQ(surfaces[0]->size(), (Size{50, 100})); // rotated, then halved
}

TEST_F(SurfaceTest, ReplacedBufferIsReleased)
{
    auto* s = makeSurface();
    auto* first = makeBuffer(10, 10);
    auto* second = makeBuffer(10, 10);

    int released = 0;
    static const wl_buffer_listener listener = {
        .release = [](void* data, wl_buffer*) { ++*static_cast<int*>(data); },
    };
    wl_buffer_add_listener(first, &listener, &released);

    wl_surface_attach(s, first, 0, 0);
    wl_surface_commit(s);
    ASSERT_TRUE(roundtrip());
    EXPECT_EQ(released, 0) << "still in use";

    wl_surface_attach(s, second, 0, 0);
    wl_surface_commit(s);
    ASSERT_TRUE(roundtrip());
    EXPECT_EQ(released, 1);
}

TEST_F(SurfaceTest, FrameCallbacksFireWhenCompositorSaysSo)
{
    auto* s = makeSurface();
    std::uint32_t doneTime = 0;
    static const wl_callback_listener listener = {
        .done = [](void* data, wl_callback* cb, std::uint32_t t) {
            *static_cast<std::uint32_t*>(data) = t;
            wl_callback_destroy(cb);
        },
    };
    wl_callback_add_listener(wl_surface_frame(s), &listener, &doneTime);
    wl_surface_commit(s);
    ASSERT_TRUE(roundtrip());
    EXPECT_EQ(doneTime, 0u);

    surfaces[0]->sendFrameDone(1234);
    ASSERT_TRUE(roundtrip());
    EXPECT_EQ(doneTime, 1234u);
}

TEST_F(SurfaceTest, OpaqueAndInputRegions)
{
    auto* s = makeSurface();
    auto* region = wl_compositor_create_region(compositor);
    wl_region_add(region, 0, 0, 100, 100);
    wl_region_subtract(region, 0, 0, 50, 100);
    wl_surface_set_opaque_region(s, region);
    wl_surface_set_input_region(s, region);
    wl_region_destroy(region); // the surface keeps a copy
    wl_surface_commit(s);
    ASSERT_TRUE(roundtrip());

    const auto& st = surfaces[0]->state();
    EXPECT_EQ(st.opaque.extents(), (Rect{50, 0, 50, 100}));
    EXPECT_FALSE(st.input.contains({10, 10}));
    EXPECT_TRUE(st.input.contains({60, 10}));
}

TEST_F(SurfaceTest, InvalidScaleIsAProtocolError)
{
    auto* s = makeSurface();
    wl_surface_set_buffer_scale(s, 0);

    EXPECT_FALSE(roundtrip());
    EXPECT_EQ(protocolError(), (std::pair<std::string, std::uint32_t>{"wl_surface", WL_SURFACE_ERROR_INVALID_SCALE}));
}

TEST_F(SurfaceTest, SyncSubsurfaceWaitsForParentCommit)
{
    auto* parent = makeSurface();
    auto* child = makeSurface();
    auto* sub = track(wl_subcompositor_get_subsurface(subcompositor, child, parent));
    wl_subsurface_set_position(sub, 10, 20);
    wl_surface_attach(child, makeBuffer(8, 8), 0, 0);
    wl_surface_commit(child);
    ASSERT_TRUE(roundtrip());

    Surface* serverChild = surfaces[1];
    ASSERT_NE(serverChild->asSubsurface(), nullptr);
    EXPECT_EQ(serverChild->buffer(), nullptr) << "synchronized: cached until the parent commits";
    EXPECT_EQ(serverChild->asSubsurface()->position(), (Point{0, 0}));

    wl_surface_commit(parent);
    ASSERT_TRUE(roundtrip());
    EXPECT_NE(serverChild->buffer(), nullptr);
    EXPECT_EQ(serverChild->asSubsurface()->position(), (Point{10, 20}));
}

TEST_F(SurfaceTest, DesyncSubsurfaceAppliesImmediately)
{
    auto* parent = makeSurface();
    auto* child = makeSurface();
    auto* sub = track(wl_subcompositor_get_subsurface(subcompositor, child, parent));
    wl_subsurface_set_desync(sub);
    wl_surface_attach(child, makeBuffer(8, 8), 0, 0);
    wl_surface_commit(child);
    ASSERT_TRUE(roundtrip());

    EXPECT_NE(surfaces[1]->buffer(), nullptr);
}

TEST_F(SurfaceTest, SwitchingToDesyncFlushesCachedState)
{
    auto* parent = makeSurface();
    auto* child = makeSurface();
    auto* sub = track(wl_subcompositor_get_subsurface(subcompositor, child, parent));
    wl_surface_attach(child, makeBuffer(8, 8), 0, 0);
    wl_surface_commit(child);
    ASSERT_TRUE(roundtrip());
    ASSERT_EQ(surfaces[1]->buffer(), nullptr);

    wl_subsurface_set_desync(sub);
    ASSERT_TRUE(roundtrip());
    EXPECT_NE(surfaces[1]->buffer(), nullptr);
}

TEST_F(SurfaceTest, SubsurfaceStackingAppliesOnParentCommit)
{
    auto* parent = makeSurface();
    auto* a = makeSurface();
    auto* b = makeSurface();
    track(wl_subcompositor_get_subsurface(subcompositor, a, parent));
    auto* subB = track(wl_subcompositor_get_subsurface(subcompositor, b, parent));
    ASSERT_TRUE(roundtrip());
    Surface *P = surfaces[0], *A = surfaces[1], *B = surfaces[2];
    EXPECT_EQ(P->stack(), (std::vector<Surface*>{P, A, B}));

    wl_subsurface_place_below(subB, parent);
    ASSERT_TRUE(roundtrip());
    EXPECT_EQ(P->stack(), (std::vector<Surface*>{P, A, B})) << "pending until the parent commits";

    wl_surface_commit(parent);
    ASSERT_TRUE(roundtrip());
    EXPECT_EQ(P->stack(), (std::vector<Surface*>{B, P, A}));
}

TEST_F(SurfaceTest, SubsurfaceCycleIsRejected)
{
    auto* a = makeSurface();
    auto* b = makeSurface();
    track(wl_subcompositor_get_subsurface(subcompositor, b, a));
    track(wl_subcompositor_get_subsurface(subcompositor, a, b)); // a would be its own grandparent

    EXPECT_FALSE(roundtrip());
    EXPECT_EQ(protocolError(),
              (std::pair<std::string, std::uint32_t>{"wl_subcompositor", WL_SUBCOMPOSITOR_ERROR_BAD_PARENT}));
}

TEST_F(SurfaceTest, DestroyingParentLeavesChildIndependent)
{
    auto* parent = makeSurface();
    auto* child = makeSurface();
    auto* sub = track(wl_subcompositor_get_subsurface(subcompositor, child, parent));
    ASSERT_TRUE(roundtrip());
    Surface* serverChild = surfaces[1];

    wl_surface_destroy(parent);
    forget(parent);
    ASSERT_TRUE(roundtrip());

    EXPECT_EQ(serverChild->asSubsurface(), nullptr) << "wl_subsurface is inert now";
    wl_subsurface_set_position(sub, 1, 1); // requests on an inert subsurface are ignored
    wl_surface_commit(child);
    EXPECT_TRUE(roundtrip());
}

TEST_F(SurfaceTest, ClientDisconnectCleansUpEverything)
{
    auto* parent = makeSurface();
    auto* child = makeSurface();
    track(wl_subcompositor_get_subsurface(subcompositor, child, parent));
    wl_surface_attach(parent, makeBuffer(16, 16), 0, 0);
    wl_surface_commit(parent);
    auto w = makeWindow();
    ASSERT_TRUE(roundtrip());
    mapWindow(*w);
    ASSERT_TRUE(roundtrip());
    ASSERT_EQ(surfaces.size(), 3u);

    disconnect();

    EXPECT_TRUE(surfaces.empty());
    EXPECT_TRUE(toplevels.empty());
}
