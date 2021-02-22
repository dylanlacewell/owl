// ======================================================================== //
// Copyright 2019-2020 Ingo Wald                                            //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

// This program sets up a single geometric object, a mesh for a cube, and
// its acceleration structure, then ray traces it.

// public owl node-graph API
#include "owl/owl.h"
// our device-side data structures
#include "deviceCode.h"
// viewer base class, for window and user interaction
#include "owlViewer/OWLViewer.h"
#include "owl/common/math/AffineSpace.h"

// VOX file reader
#include "readVox.h"
#include "ogt_vox.h"

#include <cassert>
#include <map>
#include <vector>

#define LOG(message)                                            \
  std::cout << OWL_TERMINAL_BLUE;                               \
  std::cout << "#owl.sample(main): " << message << std::endl;   \
  std::cout << OWL_TERMINAL_DEFAULT;
#define LOG_OK(message)                                         \
  std::cout << OWL_TERMINAL_LIGHT_BLUE;                         \
  std::cout << "#owl.sample(main): " << message << std::endl;   \
  std::cout << OWL_TERMINAL_DEFAULT;

extern "C" char ptxCode[];

// NOTE: the brick geometry here must lie in a unit bounding box in [0,1]x[0,1]x[0,1]
const int NUM_VERTICES = 8;
vec3f vertices[NUM_VERTICES] =
  {
    { 0.f, 0.f, 0.f },
    { 1.f, 0.f, 0.f },
    { 0.f, 1.f, 0.f },
    { 1.f, 1.f, 0.f },
    { 0.f, 0.f, 1.f },
    { 1.f, 0.f, 1.f },
    { 0.f, 1.f, 1.f },
    { 1.f, 1.f, 1.f }
  };

const int NUM_INDICES = 12;
vec3i indices[NUM_INDICES] =
  {
    { 0,1,3 }, { 2,3,0 },
    { 5,7,6 }, { 5,6,4 },
    { 0,4,5 }, { 0,5,1 },
    { 2,3,7 }, { 2,7,6 },
    { 1,5,7 }, { 1,7,3 },
    { 4,0,2 }, { 4,2,6 }
  };

// const vec2i fbSize(800,600);
const float isometricAngle = 35.564f * float(M_PI) / 180.0f;
const owl::affine3f cameraRotation = 
  owl::affine3f::rotate(vec3f(0,0,1), float(M_PI)/4.0f) *
  owl::affine3f::rotate(vec3f(-1,0,0), isometricAngle);

const vec3f init_lookFrom = xfmPoint(cameraRotation, vec3f(0, -30.f, 0));
const vec3f init_lookUp(0.f, 0.f, 1.f);

const vec3f init_lookAt {0.0f};
const float init_cosFovy = 0.10f;  // small fov to approach isometric



struct Viewer : public owl::viewer::OWLViewer
{
  Viewer(const ogt_vox_scene *scene);
  
  /*! gets called whenever the viewer needs us to re-render out widget */
  void render() override;
  
      /*! window notifies us that we got resized. We HAVE to override
          this to know our actual render dimensions, and get pointer
          to the device frame buffer that the viewer cated for us */     
  void resize(const vec2i &newSize) override;

  /*! this function gets called whenever any camera manipulator
    updates the camera. gets called AFTER all values have been updated */
  void cameraChanged() override;

  OWLGroup createInstancedTriangleGeometryScene(OWLModule module, const ogt_vox_scene *scene);
  OWLGroup createUserGeometryScene(OWLModule module, const ogt_vox_scene *scene);

  bool sbtDirty = true;
  OWLRayGen rayGen   { 0 };
  OWLLaunchParams launchParams { 0 };
  OWLContext context { 0 };
};

/*! window notifies us that we got resized */     
void Viewer::resize(const vec2i &newSize)
{
  OWLViewer::resize(newSize);
  cameraChanged();
}

/*! window notifies us that the camera has changed */
void Viewer::cameraChanged()
{
  const vec3f lookFrom = camera.getFrom();
  const vec3f lookAt = camera.getAt();
  const vec3f lookUp = camera.getUp();
  const float cosFovy = camera.getCosFovy();
  // ----------- compute variable values  ------------------
  vec3f camera_pos = lookFrom;
  vec3f camera_d00
    = normalize(lookAt-lookFrom);
  float aspect = fbSize.x / float(fbSize.y);
  vec3f camera_ddu
    = cosFovy * aspect * normalize(cross(camera_d00,lookUp));
  vec3f camera_ddv
    = cosFovy * normalize(cross(camera_ddu,camera_d00));
  camera_d00 -= 0.5f * camera_ddu;
  camera_d00 -= 0.5f * camera_ddv;

  // ----------- set variables  ----------------------------
  owlRayGenSet1ul   (rayGen,"fbPtr",        (uint64_t)fbPointer);
  // owlRayGenSetBuffer(rayGen,"fbPtr",        frameBuffer);
  owlRayGenSet2i    (rayGen,"fbSize",       (const owl2i&)fbSize);
  owlRayGenSet3f    (rayGen,"camera.pos",   (const owl3f&)camera_pos);
  owlRayGenSet3f    (rayGen,"camera.dir_00",(const owl3f&)camera_d00);
  owlRayGenSet3f    (rayGen,"camera.dir_du",(const owl3f&)camera_ddu);
  owlRayGenSet3f    (rayGen,"camera.dir_dv",(const owl3f&)camera_ddv);
  sbtDirty = true;
}

// Adapted from ogt demo code. 
// The OGT format stores voxels as solid grids; we only need to store the nonempty entries on device.

std::vector<uchar4> extractSolidVoxelsFromModel(const ogt_vox_model* model)
{
  uint32_t solid_voxel_count = 0;
  uint32_t voxel_index = 0;
  std::vector<uchar4> solid_voxels;
  solid_voxels.reserve(model->size_z * model->size_y * model->size_x);
  for (uint32_t z = 0; z < model->size_z; z++) {
    for (uint32_t y = 0; y < model->size_y; y++) {
      for (uint32_t x = 0; x < model->size_x; x++, voxel_index++) {
        // if color index == 0, this voxel is empty, otherwise it is solid.
        uint8_t color_index = model->voxel_data[voxel_index];
        bool is_voxel_solid = (color_index != 0);
        // add to our accumulator
        solid_voxel_count += (is_voxel_solid ? 1 : 0);
        if (is_voxel_solid) {
          solid_voxels.push_back(
              // Switch to Y-up
              make_uchar4(uint8_t(x), uint8_t(y), uint8_t(z), color_index));
        }
      }
    }
  }
  LOG("solid voxel count: " << solid_voxel_count);
  return solid_voxels;
}

OWLGroup Viewer::createUserGeometryScene(OWLModule module, const ogt_vox_scene *scene)
{
  // -------------------------------------------------------
  // declare user vox geometry type
  // -------------------------------------------------------

  OWLVarDecl voxGeomVars[] = {
    { "prims",  OWL_BUFPTR, OWL_OFFSETOF(VoxGeomData,prims)},
    { "colorPalette",  OWL_BUFPTR, OWL_OFFSETOF(VoxGeomData,colorPalette)},
    { /* sentinel to mark end of list */ }
  };
  OWLGeomType voxGeomType
    = owlGeomTypeCreate(context,
                        OWL_GEOMETRY_USER,
                        sizeof(VoxGeomData),
                        voxGeomVars, -1);
  owlGeomTypeSetClosestHit(voxGeomType, 0, module, "VoxGeom");
  owlGeomTypeSetIntersectProg(voxGeomType, 0, module, "VoxGeom");
  owlGeomTypeSetBoundsProg(voxGeomType, module, "VoxGeom");

  // Do this before setting up user geometry, to compile bounds program
  owlBuildPrograms(context);

  assert(scene->num_instances > 0);
  assert(scene->num_models > 0);


  // Palette buffer is global to scene
  OWLBuffer paletteBuffer
    = owlDeviceBufferCreate(context, OWL_UCHAR4, 256, scene->palette.color);

  // Cluster instances together that use the same model
  std::map<uint32_t, std::vector<uint32_t>> modelToInstances;

  for (uint32_t instanceIndex = 0; instanceIndex < scene->num_instances; instanceIndex++) {
    const ogt_vox_instance &vox_instance = scene->instances[instanceIndex];
    modelToInstances[vox_instance.model_index].push_back(instanceIndex);
  }

  std::vector<OWLGroup> geomGroups;
  geomGroups.reserve(scene->num_models);

  std::vector<owl::affine3f> instanceTransforms;
  instanceTransforms.reserve(scene->num_instances);
  owl::box3f sceneBox;
  
  // Make instance transforms
  for (auto it : modelToInstances) {

    const ogt_vox_model *vox_model = scene->models[it.first];
    assert(vox_model);
    std::vector<uchar4> voxdata = extractSolidVoxelsFromModel(vox_model);

    LOG("building user geometry for model ...");

    // ------------------------------------------------------------------
    // set up user primitives for single vox model
    // ------------------------------------------------------------------
    OWLBuffer primBuffer 
      = owlDeviceBufferCreate(context, OWL_UCHAR4, voxdata.size(), voxdata.data());

    OWLGeom voxGeom = owlGeomCreate(context, voxGeomType);
    
    owlGeomSetPrimCount(voxGeom, voxdata.size());

    owlGeomSetBuffer(voxGeom, "prims", primBuffer);
    owlGeomSetBuffer(voxGeom, "colorPalette", paletteBuffer);

    // ------------------------------------------------------------------
    // bottom level group/accel
    // ------------------------------------------------------------------
    OWLGroup userGeomGroup = owlUserGeomGroupCreate(context,1,&voxGeom);
    owlGroupBuildAccel(userGeomGroup);


    LOG("adding (" << it.second.size() << ") instance transforms for model ...");
    for (uint32_t instanceIndex : it.second) {

      const ogt_vox_instance &vox_instance = scene->instances[instanceIndex];

      const std::string instanceName = vox_instance.name ? vox_instance.name : "(unnamed)";
      if (vox_instance.hidden) {
        LOG("skipping hidden VOX instance: " << instanceName );
        continue;
      } else {
        LOG("building VOX instance: " << instanceName);
      }

      // VOX instance transform
      const ogt_vox_transform &vox_transform = vox_instance.transform;

      const owl::affine3f instanceTransform( 
          vec3f( vox_transform.m00, vox_transform.m01, vox_transform.m02 ),  // column 0
          vec3f( vox_transform.m10, vox_transform.m11, vox_transform.m12 ),  //  1
          vec3f( vox_transform.m20, vox_transform.m21, vox_transform.m22 ),  //  2
          vec3f( vox_transform.m30, vox_transform.m31, vox_transform.m32 )); //  3 

      // This matrix translates model to its center (in integer coords!) and applies instance transform
      const affine3f instanceMoveToCenterAndTransform = 
        affine3f(instanceTransform) * 
        affine3f::translate(-vec3f(float(vox_model->size_x/2),   // Note: snapping to int to match MV
                                   float(vox_model->size_y/2), 
                                   float(vox_model->size_z/2)));

      sceneBox.extend(xfmPoint(instanceMoveToCenterAndTransform, vec3f(0.0f)));
      sceneBox.extend(xfmPoint(instanceMoveToCenterAndTransform,  
            vec3f(float(vox_model->size_x), float(vox_model->size_y), float(vox_model->size_z))));

      instanceTransforms.push_back(instanceMoveToCenterAndTransform);

      geomGroups.push_back(userGeomGroup);

    }

  }

  // Apply final scene transform so we can use the same camera for every scene
  const float maxSpan = owl::reduce_max(sceneBox.span());
  const vec3f sceneCenter = sceneBox.center();
  owl::affine3f worldTransform = 
    owl::affine3f::scale(2.0f/maxSpan) *                                    // normalize
    owl::affine3f::translate(vec3f(-sceneCenter.x, -sceneCenter.y, 0.0f));  // center about (x,y) origin ,with Z up to match MV

  for (size_t i = 0; i < instanceTransforms.size(); ++i) {
    instanceTransforms[i] = worldTransform * instanceTransforms[i];
  }
  
  OWLGroup world = owlInstanceGroupCreate(context, instanceTransforms.size());
  for (size_t i = 0; i < instanceTransforms.size(); ++i) {
    owlInstanceGroupSetChild(world, i, geomGroups[i]);
    owlInstanceGroupSetTransform(world, i, (const float*)&instanceTransforms[i], OWL_MATRIX_FORMAT_OWL); 
  }

  return world;

}

OWLGroup Viewer::createInstancedTriangleGeometryScene(OWLModule module, const ogt_vox_scene *scene)
{
  // -------------------------------------------------------
  // declare triangle-based box geometry type
  // -------------------------------------------------------
  OWLVarDecl trianglesGeomVars[] = {
    { "index",  OWL_BUFPTR, OWL_OFFSETOF(TrianglesGeomData,index)},
    { "vertex", OWL_BUFPTR, OWL_OFFSETOF(TrianglesGeomData,vertex)},
    { "colorIndexPerInstance",  OWL_BUFPTR, OWL_OFFSETOF(TrianglesGeomData,colorIndexPerInstance)},
    { "colorPalette",  OWL_BUFPTR, OWL_OFFSETOF(TrianglesGeomData,colorPalette)},
    { /* sentinel to mark end of list */ }
  };
  OWLGeomType trianglesGeomType
    = owlGeomTypeCreate(context,
                        OWL_TRIANGLES,
                        sizeof(TrianglesGeomData),
                        trianglesGeomVars,-1);
  owlGeomTypeSetClosestHit(trianglesGeomType,0,
                           module,"TriangleMesh");

  LOG("building triangle geometry for single brick ...");


  // ------------------------------------------------------------------
  // triangle mesh for single unit brick
  // ------------------------------------------------------------------
  OWLBuffer vertexBuffer
    = owlDeviceBufferCreate(context,OWL_FLOAT3,NUM_VERTICES,vertices);
  OWLBuffer indexBuffer
    = owlDeviceBufferCreate(context,OWL_INT3,NUM_INDICES,indices);

  OWLGeom trianglesGeom
    = owlGeomCreate(context,trianglesGeomType);
  
  owlTrianglesSetVertices(trianglesGeom,vertexBuffer,
                          NUM_VERTICES,sizeof(vec3f),0);
  owlTrianglesSetIndices(trianglesGeom,indexBuffer,
                         NUM_INDICES,sizeof(vec3i),0);
  
  owlGeomSetBuffer(trianglesGeom,"vertex",vertexBuffer);
  owlGeomSetBuffer(trianglesGeom,"index",indexBuffer);

  OWLBuffer paletteBuffer
    = owlDeviceBufferCreate(context, OWL_UCHAR4, 256, scene->palette.color);
  owlGeomSetBuffer(trianglesGeom, "colorPalette", paletteBuffer);
  
  // ------------------------------------------------------------------
  // the group/accel for a brick
  // ------------------------------------------------------------------
  OWLGroup trianglesGroup
    = owlTrianglesGeomGroupCreate(context,1,&trianglesGeom);
  owlGroupBuildAccel(trianglesGroup);


  // ------------------------------------------------------------------
  // instance the brick for each Vox model in the scene, flattening any Vox instances
  // ------------------------------------------------------------------

  LOG("building instances ...");

  std::vector<owl::affine3f> transformsPerBrick;
  std::vector<unsigned char> colorIndicesPerBrick;
  owl::box3f sceneBox;

  assert(scene->num_instances > 0);
  assert(scene->num_models > 0);

  for (uint32_t instanceIndex = 0; instanceIndex < scene->num_instances; instanceIndex++) {

    const ogt_vox_instance &vox_instance = scene->instances[instanceIndex];

    const std::string instanceName = vox_instance.name ? vox_instance.name : "(unnamed)";
    if (vox_instance.hidden) {
      LOG("skipping hidden VOX instance: " << instanceName );
      continue;
    } else {
      LOG("building VOX instance: " << instanceName);
    }

    const ogt_vox_model *vox_model = scene->models[vox_instance.model_index];
    assert(vox_model);

    // Note: for scenes with many instances of a model, cache this or rearrange loop
    std::vector<uchar4> voxdata = extractSolidVoxelsFromModel(vox_model);

    // Color indices for this model
    for (size_t i = 0; i < voxdata.size(); ++i) {
        colorIndicesPerBrick.push_back(voxdata[i].w);
    }

    // VOX instance transform
    const ogt_vox_transform &vox_transform = vox_instance.transform;

    const owl::affine3f instanceTransform( 
        vec3f( vox_transform.m00, vox_transform.m01, vox_transform.m02 ),  // column 0
        vec3f( vox_transform.m10, vox_transform.m11, vox_transform.m12 ),  //  1
        vec3f( vox_transform.m20, vox_transform.m21, vox_transform.m22 ),  //  2
        vec3f( vox_transform.m30, vox_transform.m31, vox_transform.m32 )); //  3 

    // This matrix translates model to its center (in integer coords!) and applies instance transform
    const affine3f instanceMoveToCenterAndTransform = 
      affine3f(instanceTransform) * 
      affine3f::translate(-vec3f(float(vox_model->size_x/2),   // Note: snapping to int to match MV
                                 float(vox_model->size_y/2), 
                                 float(vox_model->size_z/2)));

    sceneBox.extend(xfmPoint(instanceMoveToCenterAndTransform, vec3f(0.0f)));
    sceneBox.extend(xfmPoint(instanceMoveToCenterAndTransform,  
          vec3f(float(vox_model->size_x), float(vox_model->size_y), float(vox_model->size_z))));

    for (size_t i = 0; i < voxdata.size(); ++i) {
        uchar4 b = voxdata[i];
        // Transform brick to its location in the scene
        owl::affine3f trans = instanceMoveToCenterAndTransform * owl::affine3f::translate(vec3f(b.x, b.y, b.z));
        transformsPerBrick.push_back(trans);
    }
  }

  // Apply final scene transform so we can use the same camera for every scene
  const float maxSpan = owl::reduce_max(sceneBox.span());
  const vec3f sceneCenter = sceneBox.center();
  owl::affine3f worldTransform = 
    owl::affine3f::scale(2.0f/maxSpan) *                                    // normalize
    owl::affine3f::translate(vec3f(-sceneCenter.x, -sceneCenter.y, 0.0f));  // center about (x,y) origin ,with Z up to match MV

  for (size_t i = 0; i < transformsPerBrick.size(); ++i) {
    transformsPerBrick[i] = worldTransform * transformsPerBrick[i];
  }

  // Set the color indices per brick now that the bricks have been fully instanced
  OWLBuffer colorIndexBuffer
      = owlDeviceBufferCreate(context, OWL_UCHAR, colorIndicesPerBrick.size(), colorIndicesPerBrick.data());
  owlGeomSetBuffer(trianglesGeom, "colorIndexPerInstance", colorIndexBuffer);

  OWLGroup world = owlInstanceGroupCreate(context, transformsPerBrick.size());

  for (int i = 0; i < int(transformsPerBrick.size()); ++i) {
    owlInstanceGroupSetChild(world, i, trianglesGroup);  // All instances point to the same brick 
    owlInstanceGroupSetTransform(world, i, (const float*)&transformsPerBrick[i], OWL_MATRIX_FORMAT_OWL);
  }

  return world;
  
}

Viewer::Viewer(const ogt_vox_scene *scene)
{
  // create a context on the first device:
  context = owlContextCreate(nullptr,1);
  OWLModule module = owlModuleCreate(context,ptxCode);

  owlContextSetRayTypeCount(context, 2);  // primary, shadow
  
  OWLGroup world = createInstancedTriangleGeometryScene(module, scene);
  //OWLGroup world = createUserGeometryScene(module, scene);

  owlGroupBuildAccel(world);
  

  // ##################################################################
  // set miss and raygen program required for SBT
  // ##################################################################

  // -------------------------------------------------------
  // set up miss progs
  // -------------------------------------------------------
  OWLVarDecl missProgVars[]
    = {
    { "color0", OWL_FLOAT3, OWL_OFFSETOF(MissProgData,color0)},
    { "color1", OWL_FLOAT3, OWL_OFFSETOF(MissProgData,color1)},
    { /* sentinel to mark end of list */ }
  };
  // ----------- create object  ----------------------------
  OWLMissProg missProg = 
    owlMissProgCreate(context,module,"miss",sizeof(MissProgData), missProgVars,-1);
  owlMissProgSet(context, 0, missProg);
  
  // ----------- set variables  ----------------------------
  owlMissProgSet3f(missProg,"color0",owl3f{.8f,0.f,0.f});
  owlMissProgSet3f(missProg,"color1",owl3f{.8f,.8f,.8f});

  // Second program for shadow visibility
  OWLMissProg shadowMissProg = 
    owlMissProgCreate(context,module,"miss_shadow", 0u, nullptr,-1);
  owlMissProgSet(context, 1, shadowMissProg);

  // -------------------------------------------------------
  // set up ray gen program
  // -------------------------------------------------------
  OWLVarDecl rayGenVars[] = {
    { "fbPtr",         OWL_RAW_POINTER, OWL_OFFSETOF(RayGenData,fbPtr)},
    // { "fbPtr",         OWL_BUFPTR, OWL_OFFSETOF(RayGenData,fbPtr)},
    { "fbSize",        OWL_INT2,   OWL_OFFSETOF(RayGenData,fbSize)},
    { "camera.pos",    OWL_FLOAT3, OWL_OFFSETOF(RayGenData,camera.pos)},
    { "camera.dir_00", OWL_FLOAT3, OWL_OFFSETOF(RayGenData,camera.dir_00)},
    { "camera.dir_du", OWL_FLOAT3, OWL_OFFSETOF(RayGenData,camera.dir_du)},
    { "camera.dir_dv", OWL_FLOAT3, OWL_OFFSETOF(RayGenData,camera.dir_dv)},
    { /* sentinel to mark end of list */ }
  };

  // ----------- create object  ----------------------------
  rayGen
    = owlRayGenCreate(context,module,"simpleRayGen",
                      sizeof(RayGenData),
                      rayGenVars,-1);
  /* camera and frame buffer get set in resiez() and cameraChanged() */

  // Launch params
  OWLVarDecl launchVars[] = {
    { "world",         OWL_GROUP,  OWL_OFFSETOF(LaunchParams,world)},
    { /* sentinel to mark end of list */ }
  };

  launchParams = 
    owlParamsCreate(context, sizeof(LaunchParams), launchVars, -1);

  owlParamsSetGroup(launchParams, "world", world);
  
  // ##################################################################
  // build *SBT* required to trace the groups
  // ##################################################################
  
  // Build all programs again, even if they have been built already for bounds program
  owlBuildPrograms(context);
  owlBuildPipeline(context);
  owlBuildSBT(context);
}

void Viewer::render()
{
  if (sbtDirty) {
    owlBuildSBT(context);
    sbtDirty = false;
  }
  //owlRayGenLaunch2D(rayGen,fbSize.x,fbSize.y, launchParams);
  owlLaunch2D(rayGen,fbSize.x,fbSize.y, launchParams);
  owlLaunchSync(launchParams);
}


int main(int ac, char **av)
{
  LOG("owl::ng example '" << av[0] << "' starting up");

  if (ac < 2) {
      LOG("missing expected argument for .vox file. Exiting");
      exit(1);
  }
  const std::string infile(av[1]);
  const ogt_vox_scene *scene = loadVoxSceneOGT(infile.c_str());

  if (!scene) {
      LOG("Could not read vox file: " << infile.c_str() << ", exiting");
      exit(1);
  }
  if (scene->num_models == 0) {
      LOG("No voxel models found in file, exiting");
      ogt_vox_destroy_scene(scene);
      exit(1);
  }

  Viewer viewer(scene);
  viewer.camera.setOrientation(init_lookFrom,
                               init_lookAt,
                               init_lookUp,
                               owl::viewer::toDegrees(acosf(init_cosFovy)));
  viewer.enableFlyMode();
  viewer.enableInspectMode(owl::box3f(vec3f(-1.f),vec3f(+1.f)));

  // ##################################################################
  // now that everything is ready: launch it ....
  // ##################################################################
  viewer.showAndRun();

  ogt_vox_destroy_scene(scene);
}