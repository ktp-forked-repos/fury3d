#include <array>
#include <cmath>
#include <unordered_map>

#include "Camera.h"
#include "Log.h"
#include "EnumUtil.h"
#include "EntityUtil.h"
#include "Frustum.h"
#include "GLLoader.h"
#include "Light.h"
#include "MathUtil.h"
#include "Material.h"
#include "Mesh.h"
#include "MeshRender.h"
#include "MeshUtil.h"
#include "Pass.h"
#include "PrelightPipeline.h"
#include "RenderQuery.h"
#include "RenderUtil.h"
#include "SceneManager.h"
#include "SceneNode.h"
#include "Shader.h"
#include "SphereBounds.h"
#include "Texture.h"

namespace fury
{
	PrelightPipeline::Ptr PrelightPipeline::Create(const std::string &name)
	{
		return std::make_shared<PrelightPipeline>(name);
	}

	PrelightPipeline::PrelightPipeline(const std::string &name)
		: Pipeline(name)
	{
		m_TypeIndex = typeid(PrelightPipeline);

		m_SharedPass = Pass::Create("SharedPass");

		m_OffsetMatrix = Matrix4({
			0.5, 0.0, 0.0, 0.0,
			0.0, 0.5, 0.0, 0.0,
			0.0, 0.0, 0.5, 0.0,
			0.5, 0.5, 0.5, 1.0
		});
	}

	void PrelightPipeline::Execute(const std::shared_ptr<SceneManager> &sceneManager)
	{
		// pre
		m_CurrentCamera = nullptr;
		m_CurrentShader = nullptr;
		SortPassByIndex();

		// find visible nodes, 1 cam 1 query
		std::unordered_map<std::string, RenderQuery::Ptr> queries;

		for (auto pair : m_PassMap)
		{
			auto pass = pair.second;
			auto camNode = pass->GetCameraNode();

			if (camNode == nullptr)
			{
				FURYW << "Camera for pass " + pass->GetName() + " not found!";
				continue;
			}

			auto it = queries.find(camNode->GetName());
			if (it != queries.end())
				continue;

			RenderQuery::Ptr query = RenderQuery::Create();

			sceneManager->GetRenderQuery(camNode->GetComponent<Camera>()->GetFrustum(), query);
			query->Sort(camNode->GetWorldPosition());

			queries.emplace(camNode->GetName(), query);
		}

		// draw passes

		for (unsigned int i = 0; i < m_SortedPasses.size(); i++)
		{
			auto passName = m_SortedPasses[i];
			auto pass = m_PassMap[passName];

			auto drawMode = pass->GetDrawMode();

			m_CurrentCamera = pass->GetCameraNode();
			m_CurrentShader = pass->GetFirstShader();

			if (m_CurrentCamera == nullptr)
				continue;

			auto query = queries[m_CurrentCamera->GetName()];

			if (drawMode == DrawMode::OPAQUE)
			{
				pass->Bind();
				for (const auto &unit : query->opaqueUnits)
					DrawUnit(pass, unit);
				pass->UnBind();
			}
			else if (drawMode == DrawMode::TRANSPARENT)
			{
				pass->Bind();
				for (const auto &unit : query->transparentUnits)
					DrawUnit(pass, unit);
				pass->UnBind();
			}
			else if (drawMode == DrawMode::QUAD)
			{
				pass->Bind();
				DrawQuad(pass);
				pass->UnBind();
			}
			else if (drawMode == DrawMode::LIGHT)
			{
				pass->Bind(true);
				//glDepthMask(GL_FALSE);

				for (const auto &node : query->lightNodes)
				{
					if (auto ptr = node->GetComponent<Light>())
					{
						if (ptr->GetType() == LightType::DIRECTIONAL)
							DrawDirLight(sceneManager, pass, node);
						else if (ptr->GetType() == LightType::POINT)
							DrawPointLight(sceneManager, pass, node);
						else
							DrawSpotLight(sceneManager, pass, node);
					}
				}
				//DrawLight(sceneManager, pass, node);

				//glDepthMask(GL_TRUE);
			}
		}

		if (m_DrawLightBounds || m_DrawOpaqueBounds)
			DrawDebug(queries);

		// post
		m_CurrentCamera = nullptr;
		m_CurrentShader = nullptr;
	}

	void PrelightPipeline::DrawUnit(const std::shared_ptr<Pass> &pass, const RenderUnit &unit)
	{
		auto node = unit.node;
		auto mesh = unit.mesh;
		auto material = unit.material;

		auto shader = material->GetShaderForPass(pass->GetRenderIndex());

		if (shader == nullptr)
			shader = pass->GetShader(mesh->IsSkinnedMesh() ? ShaderType::SKINNED_MESH : ShaderType::STATIC_MESH,
			material->GetTextureFlags());

		if (shader == nullptr)
		{
			FURYW << "Failed to draw " << node->GetName() << ", shader not found!";
			return;
		}

		shader->Bind();
		shader->BindCamera(m_CurrentCamera);
		shader->BindMatrix(Matrix4::WORLD_MATRIX, node->GetWorldMatrix());

		shader->BindMaterial(material);

		for (unsigned int i = 0; i < pass->GetTextureCount(true); i++)
		{
			auto ptr = pass->GetTextureAt(i, true);
			shader->BindTexture(ptr->GetName(), ptr);
		}

		if (mesh->GetSubMeshCount() > 0)
		{
			auto subMesh = mesh->GetSubMeshAt(unit.subMesh);
			shader->BindSubMesh(mesh, unit.subMesh);
			glDrawElements(GL_TRIANGLES, subMesh->Indices.Data.size(), GL_UNSIGNED_INT, 0);

			RenderUtil::Instance()->IncreaseTriangleCount(subMesh->Indices.Data.size());
		}
		else
		{
			shader->BindMesh(mesh);
			glDrawElements(GL_TRIANGLES, mesh->Indices.Data.size(), GL_UNSIGNED_INT, 0);

			RenderUtil::Instance()->IncreaseTriangleCount(mesh->Indices.Data.size());
		}

		shader->UnBind();

		// TODO: Maybe subMeshCount ? 
		if (mesh->IsSkinnedMesh())
			RenderUtil::Instance()->IncreaseSkinnedMeshCount();
		else
			RenderUtil::Instance()->IncreaseMeshCount();

		RenderUtil::Instance()->IncreaseDrawCall();
	}

	void PrelightPipeline::DrawPointLight(const std::shared_ptr<SceneManager> &sceneManager, const std::shared_ptr<Pass> &pass, const std::shared_ptr<SceneNode> &node)
	{
		auto light = node->GetComponent<Light>();
		auto camPtr = m_CurrentCamera->GetComponent<Camera>();
		auto camPos = m_CurrentCamera->GetWorldPosition();
		auto mesh = light->GetMesh();
		auto worldMatrix = node->GetWorldMatrix();

		Shader::Ptr shader = nullptr;
		bool castShadows = light->GetCastShadows();

		// find correct shader.
		shader = GetShaderByName(castShadows ? "pointlight_shadow_shader" : "pointlight_shader");
		if (shader == nullptr)
		{
			FURYW << "Shader for light " << node->GetName() << " not found!";
			return;
		}

		// draw shadowMap if we castShadows.
		std::pair<Texture::Ptr, Matrix4> shadowData;
		if (castShadows)
			shadowData = DrawPointLightShadowMap(sceneManager, pass, node);

		// ready to draw light volumn
		pass->Bind(false);

		// change depthTest && face culling state.
		{
			float camNear = (camPtr->GetFrustum().GetCurrentCorners()[0] - camPos).Length();
			if (SphereBounds(node->GetWorldPosition(), light->GetRadius() + camNear).IsInsideFast(camPos))
			{
				glDisable(GL_DEPTH_TEST);
				glCullFace(GL_FRONT);
			}
			else
			{
				glEnable(GL_DEPTH_TEST);
				glCullFace(GL_BACK);
			}

			worldMatrix.AppendScale(Vector4(light->GetRadius(), 0.0f));
		}
		
		shader->Bind();

		shader->BindCamera(m_CurrentCamera);
		shader->BindMatrix(Matrix4::WORLD_MATRIX, worldMatrix);

		if (castShadows && shadowData.first != nullptr)
		{
			shader->BindTexture("shadow_buffer", shadowData.first);
			shader->BindMatrix("shadow_matrix", &shadowData.second.Raw[0]);
		}

		shader->BindLight(node);
		shader->BindMesh(mesh);

		for (unsigned int i = 0; i < pass->GetTextureCount(true); i++)
		{
			auto ptr = pass->GetTextureAt(i, true);
			shader->BindTexture(ptr->GetName(), ptr);
		}

		glDrawElements(GL_TRIANGLES, mesh->Indices.Data.size(), GL_UNSIGNED_INT, 0);

		shader->UnBind();

		RenderUtil::Instance()->IncreaseDrawCall();
		RenderUtil::Instance()->IncreaseLightCount();

		pass->UnBind();

		// collect used shadow buffer
		if (castShadows)
			Texture::Pool.Collect(shadowData.first);
	}

	void PrelightPipeline::DrawDirLight(const std::shared_ptr<SceneManager> &sceneManager, const std::shared_ptr<Pass> &pass, const std::shared_ptr<SceneNode> &node)
	{
		auto light = node->GetComponent<Light>();
		auto camPtr = m_CurrentCamera->GetComponent<Camera>();
		auto camPos = m_CurrentCamera->GetWorldPosition();
		auto mesh = light->GetMesh();
		auto worldMatrix = node->GetWorldMatrix();

		Shader::Ptr shader = nullptr;
		bool castShadows = light->GetCastShadows();

		// find correct shader.
		shader = GetShaderByName(castShadows ? "dirlight_csm_shader" : "dirlight_shader");
		if (shader == nullptr)
		{
			FURYW << "Shader for light " << node->GetName() << " not found!";
			return;
		}

		// draw shadowMap if we castShadows.
		std::pair<Texture::Ptr, std::vector<Matrix4>> shadowData;
		if (castShadows)
			shadowData = DrawCascadedShadowMap(sceneManager, pass, node);

		// ready to draw light volumn
		pass->Bind(false);

		// change depthTest && face culling state.
		glEnable(GL_DEPTH_TEST);
		glCullFace(GL_BACK);

		shader->Bind();

		shader->BindCamera(m_CurrentCamera);
		shader->BindMatrix(Matrix4::WORLD_MATRIX, worldMatrix);

		if (castShadows && shadowData.first != nullptr)
		{
			shader->BindTexture("shadow_buffer", shadowData.first);

			// for cacasded shadow maps
			shader->BindMatrices("shadow_matrix", shadowData.second.size(), &shadowData.second[0]);
			float base = camPtr->GetFar() - camPtr->GetNear();
			float average = base / 4.0f;
			shader->BindFloat("shadow_far", average, average * 2, average * 3, average * 4);
		}

		shader->BindLight(node);
		shader->BindMesh(mesh);

		for (unsigned int i = 0; i < pass->GetTextureCount(true); i++)
		{
			auto ptr = pass->GetTextureAt(i, true);
			shader->BindTexture(ptr->GetName(), ptr);
		}

		glDrawElements(GL_TRIANGLES, mesh->Indices.Data.size(), GL_UNSIGNED_INT, 0);

		shader->UnBind();

		RenderUtil::Instance()->IncreaseDrawCall();
		RenderUtil::Instance()->IncreaseLightCount();

		pass->UnBind();

		// collect used shadow buffer
		if (castShadows)
			Texture::Pool.Collect(shadowData.first);
	}

	void PrelightPipeline::DrawSpotLight(const std::shared_ptr<SceneManager> &sceneManager, const std::shared_ptr<Pass> &pass, const std::shared_ptr<SceneNode> &node)
	{
		auto light = node->GetComponent<Light>();
		auto camPtr = m_CurrentCamera->GetComponent<Camera>();
		auto camPos = m_CurrentCamera->GetWorldPosition();
		auto mesh = light->GetMesh();
		auto worldMatrix = node->GetWorldMatrix();

		Shader::Ptr shader = nullptr;
		bool castShadows = light->GetCastShadows();

		// find correct shader.
		shader = GetShaderByName(castShadows ? "spotlight_shadow_shader" : "spotlight_shader");
		if (shader == nullptr)
		{
			FURYW << "Shader for light " << node->GetName() << " not found!";
			return;
		}

		// draw shadowMap if we castShadows.
		std::pair<Texture::Ptr, Matrix4> shadowData;
		if (castShadows)
			shadowData = DrawSpotLightShadowMap(sceneManager, pass, node);

		// ready to draw light volumn
		pass->Bind(false);

		// change depthTest && face culling state.
		{
			auto coneCenter = node->GetWorldPosition();
			auto coneDir = worldMatrix.Multiply(Vector4(0, -1, 0, 0)).Normalized();

			float camNear = (camPtr->GetFrustum().GetCurrentCorners()[0] - camPos).Length();
			float theta = light->GetOutterAngle() * 0.5f;
			float height = light->GetRadius();
			float extra = camNear / std::sin(theta);

			coneCenter = coneCenter - coneDir * extra;
			height += camNear + extra;

			if (MathUtil::PointInCone(coneCenter, coneDir, height, theta, camPos))
			{
				glDisable(GL_DEPTH_TEST);
				glCullFace(GL_FRONT);
			}
			else
			{
				glEnable(GL_DEPTH_TEST);
				glCullFace(GL_BACK);
			}
		}

		shader->Bind();

		shader->BindCamera(m_CurrentCamera);
		shader->BindMatrix(Matrix4::WORLD_MATRIX, worldMatrix);

		if (castShadows && shadowData.first != nullptr)
		{
			shader->BindTexture("shadow_buffer", shadowData.first);
			shader->BindMatrix("shadow_matrix", &shadowData.second.Raw[0]);
		}

		shader->BindLight(node);
		shader->BindMesh(mesh);

		for (unsigned int i = 0; i < pass->GetTextureCount(true); i++)
		{
			auto ptr = pass->GetTextureAt(i, true);
			shader->BindTexture(ptr->GetName(), ptr);
		}

		glDrawElements(GL_TRIANGLES, mesh->Indices.Data.size(), GL_UNSIGNED_INT, 0);

		shader->UnBind();

		RenderUtil::Instance()->IncreaseDrawCall();
		RenderUtil::Instance()->IncreaseLightCount();

		pass->UnBind();

		// collect used shadow buffer
		if (castShadows)
			Texture::Pool.Collect(shadowData.first);
	}

	void PrelightPipeline::DrawQuad(const std::shared_ptr<Pass> &pass)
	{
		auto shader = m_CurrentShader;
		auto mesh = MeshUtil::GetUnitQuad();

		if (shader == nullptr)
		{
			FURYW << "Failed to draw full screen quad, shader not found!";
			return;
		}

		shader->Bind();

		shader->BindMesh(mesh);
		shader->BindCamera(m_CurrentCamera);

		for (unsigned int i = 0; i < pass->GetTextureCount(true); i++)
		{
			auto ptr = pass->GetTextureAt(i, true);
			shader->BindTexture(ptr->GetName(), ptr);
		}

		glDrawElements(GL_TRIANGLES, mesh->Indices.Data.size(), GL_UNSIGNED_INT, 0);

		shader->UnBind();

		RenderUtil::Instance()->IncreaseDrawCall();
		RenderUtil::Instance()->IncreaseTriangleCount(mesh->Indices.Data.size());
	}

	std::pair<std::shared_ptr<Texture>, std::vector<Matrix4>> PrelightPipeline::DrawCascadedShadowMap(const std::shared_ptr<SceneManager> &sceneManager, const std::shared_ptr<Pass> &pass, const std::shared_ptr<SceneNode> &node)
	{
		auto depth_buffer = Texture::Pool.Get(1024, 1024, 4, TextureFormat::DEPTH24, TextureType::TEXTURE_2D_ARRAY);
		
		// for debug
		EntityUtil::Instance()->Add(depth_buffer);

		const int numSplit = 4;

		// get pointers
		auto depth_shader = GetShaderByName("leagcy_depth_shader");
		depth_buffer->SetBorderColor(Color::White);
		depth_buffer->SetWrapMode(WrapMode::CLAMP_TO_BORDER);

		auto camNode = pass->GetCameraNode();
		auto camera = camNode->GetComponent<Camera>();

		Matrix4 lightMatrix;
		lightMatrix.Rotate(MathUtil::AxisRadToQuat(Vector4::XAxis, MathUtil::DegToRad * 90.0f));
		lightMatrix = lightMatrix * node->GetInvertWorldMatrix();

		//tempFrustums.clear();

		// build frustums
		std::array<Frustum, numSplit> frustums;
		float average = (camera->GetFar() - camera->GetNear()) / (float)numSplit;
		float curNear = camera->GetNear();
		float curFar = curNear;
		for (int i = 0; i < numSplit; i++)
		{
			curFar += average;
			frustums[i] = camera->GetFrustum(curNear, curFar);
			curNear += average;
		}

		// find shadow casters
		fury::SceneManager::SceneNodes casterAll;
		sceneManager->GetVisibleShadowCasters(camera->GetFrustum(), casterAll);

		std::array<fury::SceneManager::SceneNodes, numSplit> casterArrays;
		for (int i = 0; i < numSplit; i++)
		{
			auto &casters = casterArrays[i];
			auto &frustum = frustums[i];
			MathUtil::FilterNodes(frustum, casterAll, casters);
		}

		// build projection/crop matrices
		std::array<Matrix4, numSplit> projMatrices;
		for (int i = 0; i < numSplit; i++)
		{
			auto &matrix = projMatrices[i];
			auto &frustum = frustums[i];
			auto &casters = casterArrays[i];
			matrix = MathUtil::GetCropMatrix(lightMatrix, frustum, casters);
		}

		// draw casters to depth map, aka shadow map.
		{
			m_SharedPass->RemoveAllTextures();
			m_SharedPass->AddTexture(depth_buffer, false);

			m_SharedPass->SetBlendMode(BlendMode::REPLACE);
			m_SharedPass->SetClearMode(ClearMode::COLOR_DEPTH_STENCIL);
			m_SharedPass->SetClearColor(Color::White);
			m_SharedPass->SetCompareMode(CompareMode::LESS);
			m_SharedPass->SetCullMode(CullMode::BACK);

			m_SharedPass->Bind();

			glEnable(GL_POLYGON_OFFSET_FILL);
			glPolygonOffset(2.5f, 10.0f);
			
			depth_shader->Bind();
			depth_shader->BindMatrix(Matrix4::INVERT_VIEW_MATRIX, &lightMatrix.Raw[0]);

			for (int i = 0; i < numSplit; i++)
			{
				depth_shader->BindMatrix(Matrix4::PROJECTION_MATRIX, &projMatrices[i].Raw[0]);

				m_SharedPass->SetArrayTextureLayer(i);

				auto &casters = casterArrays[i];
				for (auto &caster : casters)
				{
					auto casterRender = caster->GetComponent<MeshRender>();
					auto casterMesh = casterRender->GetMesh();

					depth_shader->BindMesh(casterMesh);
					depth_shader->BindMatrix(Matrix4::WORLD_MATRIX, &caster->GetWorldMatrix().Raw[0]);

					unsigned int subMeshCount = casterMesh->GetSubMeshCount();
					if (subMeshCount > 0)
					{
						for (unsigned int i = 0; i < subMeshCount; i++)
						{
							depth_shader->BindSubMesh(casterMesh, i);
							glDrawElements(GL_TRIANGLES, casterMesh->GetSubMeshAt(i)->Indices.Data.size(), GL_UNSIGNED_INT, 0);
							RenderUtil::Instance()->IncreaseDrawCall();
						}
					}
					else
					{
						depth_shader->BindMesh(casterMesh);
						glDrawElements(GL_TRIANGLES, casterMesh->Indices.Data.size(), GL_UNSIGNED_INT, 0);
						RenderUtil::Instance()->IncreaseDrawCall();
					}

					RenderUtil::Instance()->IncreaseTriangleCount(casterMesh->Indices.Data.size());
				}
			}
			
			glDisable(GL_POLYGON_OFFSET_FILL);
			depth_shader->UnBind();

			m_SharedPass->UnBind();
		}

		std::vector<Matrix4> matrices;
		for (int i = 0; i < numSplit; i++)
			matrices.push_back(m_OffsetMatrix * projMatrices[i] * lightMatrix * m_CurrentCamera->GetWorldMatrix());

		return std::make_pair(depth_buffer, matrices);
	}

	std::pair<std::shared_ptr<Texture>, Matrix4> PrelightPipeline::DrawDirLightShadowMap(const std::shared_ptr<SceneManager> &sceneManager, const std::shared_ptr<Pass> &pass, const std::shared_ptr<SceneNode> &node)
	{
		// get pointers
		auto depth_shader = GetShaderByName("leagcy_depth_shader");
		auto depth_buffer = Texture::Pool.Get(1024, 1024, 0, TextureFormat::DEPTH24, TextureType::TEXTURE_2D);
		depth_buffer->SetBorderColor(Color::White);
		depth_buffer->SetWrapMode(WrapMode::CLAMP_TO_BORDER);

		auto camNode = pass->GetCameraNode();
		auto camera = camNode->GetComponent<Camera>();

		Matrix4 lightMatrix;
		lightMatrix.Rotate(MathUtil::AxisRadToQuat(Vector4::XAxis, MathUtil::DegToRad * 90.0f));
		lightMatrix = lightMatrix * node->GetInvertWorldMatrix();

		// gen camera frustum
		auto camFrustum = camera->GetFrustum(camera->GetNear(), camera->GetShadowFar());

		// find shadow casters
		fury::SceneManager::SceneNodes casters;
		sceneManager->GetVisibleShadowCasters(camFrustum, casters);

		// gen projection matrix for light.
		Matrix4 projMatrix = MathUtil::GetCropMatrix(lightMatrix, camFrustum, casters);

		// draw casters to depth map, aka shadow map.
		{
			m_SharedPass->RemoveAllTextures();
			m_SharedPass->AddTexture(depth_buffer, false);

			m_SharedPass->SetBlendMode(BlendMode::REPLACE);
			m_SharedPass->SetClearMode(ClearMode::COLOR_DEPTH_STENCIL);
			m_SharedPass->SetClearColor(Color::White);
			m_SharedPass->SetCompareMode(CompareMode::LESS);
			m_SharedPass->SetCullMode(CullMode::BACK);

			m_SharedPass->Bind();

			glEnable(GL_POLYGON_OFFSET_FILL);
			glPolygonOffset(2.5f, 10.0f);

			depth_shader->Bind();
			depth_shader->BindMatrix(Matrix4::INVERT_VIEW_MATRIX, &lightMatrix.Raw[0]);
			depth_shader->BindMatrix(Matrix4::PROJECTION_MATRIX, &projMatrix.Raw[0]);

			for (auto &caster : casters)
			{
				auto casterRender = caster->GetComponent<MeshRender>();
				auto casterMesh = casterRender->GetMesh();

				depth_shader->BindMesh(casterMesh);
				depth_shader->BindMatrix(Matrix4::WORLD_MATRIX, &caster->GetWorldMatrix().Raw[0]);

				unsigned int subMeshCount = casterMesh->GetSubMeshCount();
				if (subMeshCount > 0)
				{
					for (unsigned int i = 0; i < subMeshCount; i++)
					{
						depth_shader->BindSubMesh(casterMesh, i);
						glDrawElements(GL_TRIANGLES, casterMesh->GetSubMeshAt(i)->Indices.Data.size(), GL_UNSIGNED_INT, 0);
						RenderUtil::Instance()->IncreaseDrawCall();
					}
				}
				else
				{
					depth_shader->BindMesh(casterMesh);
					glDrawElements(GL_TRIANGLES, casterMesh->Indices.Data.size(), GL_UNSIGNED_INT, 0);
					RenderUtil::Instance()->IncreaseDrawCall();
				}

				RenderUtil::Instance()->IncreaseTriangleCount(casterMesh->Indices.Data.size());
			}

			glDisable(GL_POLYGON_OFFSET_FILL);
			depth_shader->UnBind();

			m_SharedPass->UnBind();
		}

		return std::make_pair(depth_buffer, m_OffsetMatrix * projMatrix * lightMatrix * m_CurrentCamera->GetWorldMatrix());
	}

	std::pair<std::shared_ptr<Texture>, Matrix4> PrelightPipeline::DrawPointLightShadowMap(const std::shared_ptr<SceneManager> &sceneManager, const std::shared_ptr<Pass> &pass, const std::shared_ptr<SceneNode> &node)
	{
		auto depth_shader = GetShaderByName("cube_depth_shader");
		auto depth_buffer = Texture::Pool.Get(512, 512, 0, TextureFormat::DEPTH24, TextureType::TEXTURE_CUBE_MAP);

		auto camNode = pass->GetCameraNode();
		auto camera = camNode->GetComponent<Camera>();

		auto light = node->GetComponent<Light>();
		auto radius = light->GetRadius();
		auto lightSphere = SphereBounds(node->GetWorldPosition(), radius);

		// TODO: filter casters for all six directions.
		fury::SceneManager::SceneNodes casters;
		sceneManager->GetVisibleShadowCasters(lightSphere, casters);

		float aspect = (float)depth_buffer->GetWidth() / depth_buffer->GetHeight();
		Matrix4 projMatrix;
		projMatrix.PerspectiveFov(MathUtil::DegToRad * 90.0f, aspect, 1.0f, radius);

		// dir matrices that points camera to all 6 directions.
		// right, left, top, bottom, back, front
		std::array<Matrix4, 6> dirMatrices;

		auto lightPos = node->GetWorldPosition();
		dirMatrices[0].LookAt(lightPos, lightPos + Vector4(1.0f, 0.0f, 0.0f), Vector4(0.0f, -1.0f, 0.0f));
		dirMatrices[1].LookAt(lightPos, lightPos + Vector4(-1.0f, 0.0f, 0.0f), Vector4(0.0f, -1.0f, 0.0f));
		dirMatrices[2].LookAt(lightPos, lightPos + Vector4(0.0f, 1.0f, 0.0f), Vector4(0.0f, 0.0f, 1.0f));
		dirMatrices[3].LookAt(lightPos, lightPos + Vector4(0.0f, -1.0f, 0.0f), Vector4(0.0f, 0.0f, -1.0f));
		dirMatrices[4].LookAt(lightPos, lightPos + Vector4(0.0f, 0.0f, 1.0f), Vector4(0.0f, -1.0f, 0.0f));
		dirMatrices[5].LookAt(lightPos, lightPos + Vector4(0.0f, 0.0f, -1.0f), Vector4(0.0f, -1.0f, 0.0f));

		// draw casters to depth map, aka shadow map.
		{
			m_SharedPass->RemoveAllTextures();
			m_SharedPass->AddTexture(depth_buffer, false);

			m_SharedPass->SetBlendMode(BlendMode::REPLACE);
			m_SharedPass->SetClearMode(ClearMode::COLOR_DEPTH_STENCIL);
			m_SharedPass->SetClearColor(Color::White);
			m_SharedPass->SetCompareMode(CompareMode::LESS);
			m_SharedPass->SetCullMode(CullMode::BACK);

			m_SharedPass->Bind();

			glEnable(GL_POLYGON_OFFSET_FILL);
			glPolygonOffset(2.5f, 10.0f);

			depth_shader->Bind();
			depth_shader->BindMatrix(Matrix4::PROJECTION_MATRIX, &projMatrix.Raw[0]);
			depth_shader->BindFloat("light_far", radius);
			depth_shader->BindFloat("light_pos", lightPos.x, lightPos.y, lightPos.z);

			for (int i = 0; i < 6; i++)
			{
				// TODO: test if it's necessary to clear after attach new cubemap face.
				m_SharedPass->SetCubeTextureIndex(i);
				m_SharedPass->Clear(m_SharedPass->GetClearMode(), m_SharedPass->GetClearColor());

				for (auto &caster : casters)
				{
					auto casterRender = caster->GetComponent<MeshRender>();
					auto casterMesh = casterRender->GetMesh();

					auto ivm = dirMatrices[i];

					depth_shader->BindMesh(casterMesh);
					depth_shader->BindMatrix(Matrix4::INVERT_VIEW_MATRIX, &ivm.Raw[0]);
					depth_shader->BindMatrix(Matrix4::WORLD_MATRIX, &caster->GetWorldMatrix().Raw[0]);

					unsigned int subMeshCount = casterMesh->GetSubMeshCount();
					if (subMeshCount > 0)
					{
						for (unsigned int i = 0; i < subMeshCount; i++)
						{
							depth_shader->BindSubMesh(casterMesh, i);
							glDrawElements(GL_TRIANGLES, casterMesh->GetSubMeshAt(i)->Indices.Data.size(), GL_UNSIGNED_INT, 0);
							RenderUtil::Instance()->IncreaseDrawCall();
						}
					}
					else
					{
						depth_shader->BindMesh(casterMesh);
						glDrawElements(GL_TRIANGLES, casterMesh->Indices.Data.size(), GL_UNSIGNED_INT, 0);
						RenderUtil::Instance()->IncreaseDrawCall();
					}

					RenderUtil::Instance()->IncreaseTriangleCount(casterMesh->Indices.Data.size());
				}
			}

			glDisable(GL_POLYGON_OFFSET_FILL);
			depth_shader->UnBind();

			m_SharedPass->UnBind();
		}

		return std::make_pair(depth_buffer, m_CurrentCamera->GetWorldMatrix());
	}

	std::pair<std::shared_ptr<Texture>, Matrix4> PrelightPipeline::DrawSpotLightShadowMap(const std::shared_ptr<SceneManager> &sceneManager, const std::shared_ptr<Pass> &pass, const std::shared_ptr<SceneNode> &node)
	{
		// get pointers
		auto depth_shader = GetShaderByName("leagcy_depth_shader");
		auto depth_buffer = Texture::Pool.Get(1024, 1024, 0, TextureFormat::DEPTH24, TextureType::TEXTURE_2D);

		// for debug
		EntityUtil::Instance()->Add(depth_buffer);

		depth_buffer->SetBorderColor(Color::White);
		depth_buffer->SetWrapMode(WrapMode::CLAMP_TO_BORDER);

		auto light = node->GetComponent<Light>();
		auto radius = light->GetRadius();

		Matrix4 lightMatrix;
		lightMatrix.Rotate(MathUtil::AxisRadToQuat(Vector4::XAxis, MathUtil::DegToRad * 90.0f));
		lightMatrix = lightMatrix * node->GetInvertWorldMatrix();

		Frustum frustum;
		frustum.Setup(light->GetOutterAngle(), 1.0f, 1.0f, light->GetRadius());
		frustum.Transform(lightMatrix.Inverse());

		// gen projection matrix for light.
		float aspect = (float)depth_buffer->GetWidth() / depth_buffer->GetHeight();
		Matrix4 projMatrix;
		projMatrix.PerspectiveFov(light->GetOutterAngle(), aspect, 1.0f, radius);

		// find shadow casters
		fury::SceneManager::SceneNodes casters;
		sceneManager->GetVisibleRenderables(frustum, casters);

		// draw casters to depth map, aka shadow map.
		{
			m_SharedPass->RemoveAllTextures();
			m_SharedPass->AddTexture(depth_buffer, false);

			m_SharedPass->SetBlendMode(BlendMode::REPLACE);
			m_SharedPass->SetClearMode(ClearMode::COLOR_DEPTH_STENCIL);
			m_SharedPass->SetClearColor(Color::White);
			m_SharedPass->SetCompareMode(CompareMode::LESS);
			m_SharedPass->SetCullMode(CullMode::BACK);

			m_SharedPass->Bind();

			/*glEnable(GL_POLYGON_OFFSET_FILL);
			glPolygonOffset(2.5f, 20.0f);*/

			depth_shader->Bind();
			depth_shader->BindMatrix(Matrix4::INVERT_VIEW_MATRIX, &lightMatrix.Raw[0]);
			depth_shader->BindMatrix(Matrix4::PROJECTION_MATRIX, &projMatrix.Raw[0]);

			for (auto &caster : casters)
			{
				auto casterRender = caster->GetComponent<MeshRender>();
				auto casterMesh = casterRender->GetMesh();

				depth_shader->BindMesh(casterMesh);
				depth_shader->BindMatrix(Matrix4::WORLD_MATRIX, &caster->GetWorldMatrix().Raw[0]);

				unsigned int subMeshCount = casterMesh->GetSubMeshCount();
				if (subMeshCount > 0)
				{
					for (unsigned int i = 0; i < subMeshCount; i++)
					{
						depth_shader->BindSubMesh(casterMesh, i);
						glDrawElements(GL_TRIANGLES, casterMesh->GetSubMeshAt(i)->Indices.Data.size(), GL_UNSIGNED_INT, 0);
						RenderUtil::Instance()->IncreaseDrawCall();
					}
				}
				else
				{
					depth_shader->BindMesh(casterMesh);
					glDrawElements(GL_TRIANGLES, casterMesh->Indices.Data.size(), GL_UNSIGNED_INT, 0);
					RenderUtil::Instance()->IncreaseDrawCall();
				}

				RenderUtil::Instance()->IncreaseTriangleCount(casterMesh->Indices.Data.size());
			}

			//glDisable(GL_POLYGON_OFFSET_FILL);
			depth_shader->UnBind();

			m_SharedPass->UnBind();
		}

		return std::make_pair(depth_buffer, m_OffsetMatrix * projMatrix * lightMatrix * m_CurrentCamera->GetWorldMatrix());
	}

	void PrelightPipeline::DrawDebug(std::unordered_map<std::string, RenderQuery::Ptr> &queries)
	{
		glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

		glEnable(GL_DEPTH_TEST);
		glEnable(GL_CULL_FACE);
		glCullFace(GL_BACK);
		glDisable(GL_BLEND);

		auto renderUtil = RenderUtil::Instance();

		for (auto &pair : m_PassMap)
		{
			auto pass = pair.second;
			auto camNode = pass->GetCameraNode();

			if (camNode == nullptr || pass->GetDrawMode() == DrawMode::QUAD)
				continue;

			auto it = queries.find(camNode->GetName());
			if (it == queries.end())
				continue;

			auto visibles = it->second;
			renderUtil->BeginDrawLines(camNode);

			if (m_DrawOpaqueBounds)
			{
				for (auto node : visibles->renderableNodes)
					renderUtil->DrawBoxBounds(node->GetWorldAABB(), Color::White);
			}

			renderUtil->EndDrawLines();

			renderUtil->BeginDrawMeshs(camNode);

			if (m_DrawLightBounds)
			{
				for (auto node : visibles->lightNodes)
				{
					auto light = node->GetComponent<Light>();
					if (light->GetType() == LightType::SPOT)
					{
						renderUtil->DrawMesh(light->GetMesh(), node->GetWorldMatrix(), light->GetColor());
					}
					else if (light->GetType() == LightType::POINT)
					{
						Matrix4 worldMatrix = node->GetWorldMatrix();
						worldMatrix.AppendScale(Vector4(light->GetRadius(), 0.0f));
						renderUtil->DrawMesh(light->GetMesh(), worldMatrix, light->GetColor());
					}
				}
			}

			renderUtil->EndDrawMeshes();
		}

		glDisable(GL_DEPTH_TEST);
	}
}