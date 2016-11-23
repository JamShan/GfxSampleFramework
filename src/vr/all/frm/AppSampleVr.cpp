#include <frm/AppSampleVr.h>

#include <frm/def.h>
#include <frm/Framebuffer.h>
#include <frm/GlContext.h>
#include <frm/Profiler.h>
#include <frm/Shader.h>
#include <frm/Texture.h>
#include <frm/ui/TextureViewer.h>

#include <OVR_CAPI.h>
#include <OVR_CAPI_GL.h>

using namespace frm;
using namespace apt;

static ovrErrorInfo s_errInfo;
static const char* OvrErrorString()
{
	ovr_GetLastErrorInfo(&s_errInfo);
	return s_errInfo.ErrorString;
}
static ovrResult OvrErrorResult()
{
	ovr_GetLastErrorInfo(&s_errInfo);
	return s_errInfo.Result;
}

#define ovrAssert(_call) \
	if (OVR_FAILURE((_call))) { \
		APT_LOG_ERR("%s", OvrErrorString()); \
		APT_ASSERT(false); \
	}
static mat4 OvrPoseToMat4(const ovrPosef& _pose)
{
	const ovrQuatf& q = _pose.Orientation;
	const ovrVector3f& p = _pose.Position;
	return translate(mat4(1.0), vec3(p.x, p.y, p.z)) * mat4_cast(quat(q.w, q.x, q.y, q.z));
}
static mat4 OvrMatrixToMat4(const ovrMatrix4f& _mat)
{
	return mat4(
		_mat.M[0][0], _mat.M[1][0], _mat.M[2][0], _mat.M[3][0],
		_mat.M[0][1], _mat.M[1][1], _mat.M[2][1], _mat.M[3][1],
		_mat.M[0][2], _mat.M[1][2], _mat.M[2][2], _mat.M[3][2],
		_mat.M[0][3], _mat.M[1][3], _mat.M[2][3], _mat.M[3][3]
		);
}
struct OvrLayer
{
	ovrLayerEyeFov      m_ovrLayer;
	ovrTextureSwapChain m_ovrSwapChain[frm::AppSampleVr::kEyeCount]; // swap chains are per-eye per layer
	int                 m_swapChainLength;
	int                 m_currentSwapChainIndex[frm::AppSampleVr::kEyeCount];
	frm::Texture        **m_chainTextures[frm::AppSampleVr::kEyeCount]; // swapChainLength proxy textures per eye
	frm::Framebuffer    **m_chainFramebuffers[frm::AppSampleVr::kEyeCount]; // swapChainLength proxy framebuffers per eye
};
struct frm::AppSampleVr::VrContext
{
 // init
	ovrSession          m_ovrSession;
	ovrGraphicsLuid     m_ovrGraphicsLuid;
	ovrHmdDesc          m_ovrHmdDesc;
	ovrTrackerDesc      m_ovrTrackerDesc[4];
	ovrEyeRenderDesc    m_ovrEyeDesc[kEyeCount];
	ovrMirrorTexture    m_ovrMirrorTexture;

 // per frame
	ovrTrackingState    m_ovrTrackingState;
	ovrPosef            m_ovrEyePose[kEyeCount];
	OvrLayer            m_layers[kLayerCount];

};

// PUBLIC

bool AppSampleVr::init(const apt::ArgList& _args)
{
	if (!AppSample3d::init(_args)) {
		return false;
	}
	getGlContext()->setVsyncMode(GlContext::VsyncMode::kOff);

	m_vrMode = true;
	m_vrCtx = new VrContext;
	if (!initVr()) {
		return false;
	}

	m_eyeFovScale = 1.0f;
	m_clipNear = 0.05f;
	m_clipFar = 1000.0f;
	m_nodeHead = m_scene.createNode("Head", Node::kTypeRoot);
	pollHmd(); // update head position

	m_shHmdMirror = Shader::CreateVsFs("shaders/Basic_vs.glsl", "shaders/Basic_fs.glsl");
	if (!m_shHmdMirror) return false;

	m_shVuiScreen = Shader::CreateVsFs("shaders/VuiScreen_vs.glsl", "shaders/VuiScreen_fs.glsl");
	if (!m_shVuiScreen) return false;
	m_vuiScreenPlane = Plane(normalize(GetTranslation(m_nodeHead->getLocalMatrix()) - *m_vuiScreenOrigin), *m_vuiScreenOrigin);

	m_txVuiScreen = Texture::Create2d(1920, 1080, GL_RGBA8, APT_MIN(Texture::GetMaxMipCount(1920, 1080), 8));
	if (!m_txVuiScreen) return false;
	m_txVuiScreen->setWrap(GL_CLAMP_TO_EDGE);

	//m_txVuiScreenButtonMove = Texture::Create("textures/button_move.dds");
	//if (!m_txVuiScreenButtonMove) return false;
	//m_txVuiScreenButtonScale = Texture::Create("textures/button_scale.dds");
	//if (!m_txVuiScreenButtonScale) return false;
	//m_txVuiScreenButtonDistance = Texture::Create("textures/button_distance.dds");
	//if (!m_txVuiScreenButtonDistance) return false;

	m_fbVuiScreen = Framebuffer::Create(1, m_txVuiScreen);
	if (!m_fbVuiScreen) return false;

	return true;
}

void AppSampleVr::shutdown()
{
	delete m_vrCtx;
	AppSample3d::shutdown();
}

bool AppSampleVr::update()
{
	if (!AppSample3d::update()) {
		return false;
	}


	AUTO_MARKER("AppSampleVR::update");
	ovrSessionStatus sessionStatus;
	ovr_GetSessionStatus(m_vrCtx->m_ovrSession, &sessionStatus);
	if (sessionStatus.ShouldQuit) {
		return false;
	}
	if (sessionStatus.DisplayLost) {
		APT_LOG("VR Display lost");
		shutdownVr();
		if (!initVr()) {
			APT_LOG("Failed to reacquire display, switching to non-VR mode");
			m_vrMode = false; // \todo re-enter VR mode if the HMD is reconnected?
			return true;
		}
	}
	if (sessionStatus.ShouldRecenter) {
		// \todo
	}
	m_vrMode = sessionStatus.IsVisible != 0;

	ImGuiIO& io = ImGui::GetIO();
	io.FontGlobalScale = 1.0f; // 1 for non-vr mode
	if (!m_vrMode) {
		return true;
	}
	io.FontGlobalScale = 2.0f;

	ImGui::Begin("##vuiscreenmove", 0,
		ImGuiWindowFlags_NoTitleBar |
		ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_AlwaysAutoResize
		);
		static TextureView moveTxView(m_txVuiScreenButtonMove);
		static TextureView scaleTxView(m_txVuiScreenButtonScale);
		static TextureView distTxView(m_txVuiScreenButtonDistance);

		const vec2 kButtonSize(64.0f);
		if (ImGui::ImageButton((ImTextureID)&moveTxView, kButtonSize, ImVec2(0, 0), ImVec2(1, 1), 0)) {
			m_moveVuiScreen = true;
		}
		ImGui::SameLine();
		if (ImGui::ImageButton((ImTextureID)&scaleTxView, kButtonSize, ImVec2(0, 0), ImVec2(1, 1), 0)) {
			m_scaleVuiScreen = true;
		}
		ImGui::SameLine();
		if (ImGui::ImageButton((ImTextureID)&distTxView, kButtonSize, ImVec2(0, 0), ImVec2(1, 1), 0)) {
			m_distVuiScreen = true;
		}
	ImGui::End();

	if (m_moveVuiScreen) {
		if (io.MouseClicked[0]) {
			m_moveVuiScreen = false;
		}
		Ray r = getCursorRayW();
		*m_vuiScreenOrigin = r.m_origin + r.m_direction * *m_vuiScreenDistance;
		m_vuiScreenPlane = Plane(-r.m_direction, *m_vuiScreenOrigin);
	}
	if (m_scaleVuiScreen) {
		if (io.MouseClicked[0]) {
			m_scaleVuiScreen = false;
		}
		*m_vuiScreenSize += m_headRotationDelta.y * 20.0f;
		*m_vuiScreenSize = APT_CLAMP(*m_vuiScreenSize, 0.001f, 100.0f);
	}
	if (m_distVuiScreen) {
		if (io.MouseClicked[0]) {
			m_distVuiScreen = false;
		}
		*m_vuiScreenDistance += m_headRotationDelta.y * 20.0f;
		*m_vuiScreenDistance= APT_CLAMP(*m_vuiScreenDistance, 0.1f, 1000.0f); // todo - get the min/max from the properties
		*m_vuiScreenOrigin = GetTranslation(m_nodeHead->getWorldMatrix()) - m_vuiScreenPlane.m_normal * *m_vuiScreenDistance;
		m_vuiScreenPlane = Plane(m_vuiScreenPlane.m_normal, *m_vuiScreenOrigin);
	}

 // update mipmap for txVuiScreen (rendered last frame)
	m_vuiScreenWorldMatrix = LookAt(*m_vuiScreenOrigin, *m_vuiScreenOrigin + m_vuiScreenPlane.m_normal);
	m_txVuiScreen->generateMipmap();

	for (int i = 0; i < kLayerCount; ++i) {
		for (int j = 0; j < kEyeCount; ++j) {
			OvrLayer& layer = m_vrCtx->m_layers[i];
			ovrAssert(ovr_GetTextureSwapChainCurrentIndex(m_vrCtx->m_ovrSession, layer.m_ovrSwapChain[j], &layer.m_currentSwapChainIndex[j]));
			#ifdef APT_DEBUG
			 // validate that the texture wasn't changed since initVr()
				int ln;
				ovrAssert(ovr_GetTextureSwapChainLength(m_vrCtx->m_ovrSession, layer.m_ovrSwapChain[j], &ln));
				APT_ASSERT(layer.m_swapChainLength == ln);
				for (int k = 0; k < layer.m_swapChainLength; ++k) {
					GLuint txH;
					ovrAssert(ovr_GetTextureSwapChainBufferGL(m_vrCtx->m_ovrSession, layer.m_ovrSwapChain[j], k, &txH));
					APT_ASSERT(layer.m_chainTextures[j][k]->getHandle() == txH);
				}
				
			#endif
		}
	}

	GlContext* ctx = getGlContext();
	for (int i = 0; i < kEyeCount; ++i) {
		ctx->setFramebufferAndViewport(getEyeFramebuffer((Eye)i, kLayerText));
		glAssert(glClearColor(0.0f, 0.0f, 0.0f, 0.0f));
		glAssert(glClear(GL_COLOR_BUFFER_BIT));
	}
	pollHmd(); // poll as late as possible

	return true;
}

void AppSampleVr::drawMainMenuBar()
{
	AppSample3d::drawMainMenuBar();
	if (ImGui::BeginMenu("VR")) {
		if (ImGui::MenuItem("Options")) {
			m_showVrOptions = !m_showVrOptions;
		}		

		ImGui::EndMenu();
	}

	ImGui::SameLine();
}

void AppSampleVr::drawStatusBar()
{
	AppSample3d::drawStatusBar();

	if (m_vrCtx) {
		bool orientationTracked = (m_vrCtx->m_ovrTrackingState.StatusFlags & ovrStatus_OrientationTracked) != 0;
		bool positionTracked = (m_vrCtx->m_ovrTrackingState.StatusFlags & ovrStatus_PositionTracked) != 0;
		ImGui::TextColored(ImColor(positionTracked ? IM_COL32_GREEN : IM_COL32_RED), " POSITION ");
		ImGui::SameLine();
		ImGui::TextColored(ImColor(orientationTracked ? IM_COL32_GREEN : IM_COL32_RED), "ORIENTATION ");
		ImGui::SameLine();
		ImGui::TextColored(ImColor(m_disableRender ? IM_COL32_RED : IM_COL32_GREEN), "RENDER");
		ImGui::SameLine();
	}
}

void AppSampleVr::draw()
{
	GlContext* ctx = getGlContext();

	if (m_vrMode) {
		setDefaultFramebuffer(m_fbVuiScreen);
		ctx->setFramebufferAndViewport(m_fbVuiScreen);
		glAssert(glClearColor(0.0f, 0.0f, 0.0f, 0.0f));
		glAssert(glClear(GL_COLOR_BUFFER_BIT));

		{	AUTO_MARKER("ovr_SubmitFrame");
			ovrLayerHeader* layers[kLayerCount] = {};
			for (int i = 0; i < kLayerCount; ++i) {
				layers[i] = &m_vrCtx->m_layers[i].m_ovrLayer.Header;
			};
			ovrResult submitRet;
			ovrAssert(submitRet = ovr_SubmitFrame(m_vrCtx->m_ovrSession, (long long)getFrameIndex(), 0, layers, kLayerCount));
			m_disableRender = submitRet == ovrSuccess_NotVisible;
		}

	 // draw mirror texture to the viewport
		ctx->setFramebufferAndViewport(0);
		ctx->setShader(m_shHmdMirror);
		ctx->bindTexture("txTexture2d", m_txHmdMirror);
		drawNdcQuad();
	} else {
		setDefaultFramebuffer(0); // default framebuffer is window
		drawVuiScreen(*m_scene.getDrawCamera());
	}

	AppSample3d::draw();
}

Ray AppSampleVr::getCursorRayW() const
{
	if (m_vrMode) {
		return Ray(
			GetTranslation(m_nodeHead->getWorldMatrix()),
			-normalize(vec3(column(m_nodeHead->getWorldMatrix(), 2)))
			);
	} else {
		return AppSample3d::getCursorRayW();
	}
}
Ray AppSampleVr::getCursorRayV() const
{
	if (m_vrMode) {
		return Ray(vec3(0.0), vec3(0.0f, 0.0f, -1.0f));
	} else {
		return AppSample3d::getCursorRayV();
	}
}


// PROTECTED

AppSampleVr::AppSampleVr(const char* _title, const char* _appDataPath)
	: AppSample3d(_title, _appDataPath)
	, m_vrCtx(0)
	, m_txVuiScreen(0)
	, m_fbVuiScreen(0)
	, m_vuiScreenDistance(0)
	, m_vuiScreenSize(0)
	, m_vuiScreenAspect(0)
	, m_moveVuiScreen(false)
	, m_scaleVuiScreen(false)
	, m_distVuiScreen(false)
	, m_showVuiScreen(true)
	, m_showVrOptions(false)
{

	AppPropertyGroup& props = m_properties.addGroup("AppSampleVr");
	//             name                   display name              default                    min        max      hidden
	props.addVec3 ("VuiScreenOrigin",     "Vui Screen Origin",      vec3(0.0f, 1.0f, -1.0f),  -1000.0f,  1000.0f,  false);
	props.addFloat("VuiScreenDistance",   "Vui Screen Distance",    2.0f,                     0.0f,      1000.0f,  false);
	props.addFloat("VuiScreenSize",       "Vui Screen Size",        0.4f,                     0.1f,      1000.0f,  false);
	props.addFloat("VuiScreenAspect",     "Vui Screen Aspect",      16.0f/9.0f,               0.5f,      4.0f,     false);

	m_vuiScreenOrigin    = &props["VuiScreenOrigin"].getValue<vec3>();
	m_vuiScreenDistance  = &props["VuiScreenDistance"].getValue<float>();
	m_vuiScreenSize      = &props["VuiScreenSize"].getValue<float>();
	m_vuiScreenAspect    = &props["VuiScreenAspect"].getValue<float>();

}

AppSampleVr::~AppSampleVr()
{
}

void AppSampleVr::ImGui_OverrideIo()
{
	ImGuiIO& io = ImGui::GetIO();
	// \todo acquire tracking data for 3d cursor / UI cursor (or use last frame's = probably fine).
	// \todo apply modifications to ImGui here? seems to work, but why?
	// \todo also handle ovr remote input here (modify gamepad 0 state?)
	// \todo don't render ImGui when the screen isn't visible
	if (m_vrMode) {
		io.MouseDrawCursor = false;

	 // gaze cursor -> vrui screen
		Ray gazeW = getCursorRayW();
		float tnear;
		if (gazeW.intersect(m_vuiScreenPlane, tnear)) {
			vec3 pw = gazeW.m_origin + gazeW.m_direction * tnear;
			vec4 pndc = inverse(m_vuiScreenWorldMatrix) * vec4(pw, 1.0f);
			vec2 sz(*m_vuiScreenSize * *m_vuiScreenAspect, *m_vuiScreenSize);
			pndc.x /= sz.x;
			pndc.y /= sz.y;
			if ((abs(pndc.x) < 1.0f) && (abs(pndc.y) < 1.0f)) {
				io.MouseDrawCursor = true;
				io.MousePos.x = (pndc.x * 0.5f + 0.5f) * (float)m_txVuiScreen->getWidth();
				io.MousePos.y = (-pndc.y * 0.5f + 0.5f) * (float)m_txVuiScreen->getHeight();
			}
		}
	}


}

const Texture* AppSampleVr::getEyeTexture(Eye _eye, Layer _layer)
{
	OvrLayer& layer = m_vrCtx->m_layers[_layer];
	return layer.m_chainTextures[_eye][layer.m_currentSwapChainIndex[_eye]];
}

const Framebuffer* AppSampleVr::getEyeFramebuffer(Eye _eye, Layer _layer)
{
	OvrLayer& layer = m_vrCtx->m_layers[_layer];
	return layer.m_chainFramebuffers[_eye][layer.m_currentSwapChainIndex[_eye]];
}

void AppSampleVr::commitEye(Eye _eye, Layer _layer)
{
	if (_layer == kLayerText) {
	// render UI/Im3d
		GlContext* ctx = getGlContext();
		const Camera& cam = m_eyeCameras[_eye];
		ctx->setFramebufferAndViewport(getEyeFramebuffer(_eye, kLayerText));
		Im3d_Render(Im3d::GetCurrentContext(), cam);
		drawVuiScreen(cam);
		const_cast<Texture*>(getEyeTexture(_eye, kLayerText))->generateMipmap();
	}

	ovrAssert(ovr_CommitTextureSwapChain(m_vrCtx->m_ovrSession, m_vrCtx->m_layers[_layer].m_ovrSwapChain[_eye]));
}

void AppSampleVr::pollHmd()
{
	ovrTrackingState& trackState = m_vrCtx->m_ovrTrackingState;
	double predictedDisplayTime = ovr_GetPredictedDisplayTime(m_vrCtx->m_ovrSession, (long long)getFrameIndex());
	trackState = ovr_GetTrackingState(m_vrCtx->m_ovrSession, predictedDisplayTime, ovrTrue);
	
	if (trackState.StatusFlags & (ovrStatus_OrientationTracked | ovrStatus_PositionTracked)) {
	 // position or orientation changed, update poses
		
		double sampleTime = ovr_GetTimeInSeconds();
		mat4 headMat = OvrPoseToMat4(trackState.HeadPose.ThePose);
		float oldHeadPitch = dot(vec3(column(m_nodeHead->getWorldMatrix(), 3)), vec3(0.0f, 1.0f, 0.0));
		float newHeadPitch = dot(vec3(column(headMat, 3)), vec3(0.0f, 1.0f, 0.0));
		m_headRotationDelta.y = newHeadPitch - oldHeadPitch;
		float oldHeadYaw = dot(vec3(column(m_nodeHead->getWorldMatrix(), 3)), vec3(0.0f, 0.0f, 1.0));
		float newHeadYaw = dot(vec3(column(headMat, 3)), vec3(0.0f, 0.0f, 1.0));
		m_headRotationDelta.x = newHeadYaw - oldHeadYaw;
		m_nodeHead->setLocalMatrix(headMat);

		// re-acquire eye render descs (IPD can change during runtime)
		for (int i = 0; i < kEyeCount; ++i) {
			m_vrCtx->m_ovrEyeDesc[i] = ovr_GetRenderDesc(m_vrCtx->m_ovrSession, (ovrEyeType)i, m_vrCtx->m_ovrHmdDesc.DefaultEyeFov[i]);
		}

		// calculate eye poses
		ovrVector3f hmdToEyeOffset[kEyeCount] = {
			m_vrCtx->m_ovrEyeDesc[0].HmdToEyeOffset,
			m_vrCtx->m_ovrEyeDesc[1].HmdToEyeOffset
			};
		ovr_CalcEyePoses(trackState.HeadPose.ThePose, hmdToEyeOffset, m_vrCtx->m_ovrEyePose);

		// build eye cameras
		for (int i = 0; i < kEyeCount; ++i) {
			const ovrEyeRenderDesc& eyeDesc = m_vrCtx->m_ovrEyeDesc[i];
			const ovrPosef& eyePose = m_vrCtx->m_ovrEyePose[i];
			m_eyeCameras[i].setTanFovUp(eyeDesc.Fov.UpTan * m_eyeFovScale);
			m_eyeCameras[i].setTanFovDown(eyeDesc.Fov.DownTan * m_eyeFovScale);
			m_eyeCameras[i].setTanFovLeft(eyeDesc.Fov.LeftTan * m_eyeFovScale);
			m_eyeCameras[i].setTanFovRight(eyeDesc.Fov.RightTan * m_eyeFovScale);
			m_eyeCameras[i].setWorldMatrix(OvrPoseToMat4(eyePose));
			m_eyeCameras[i].build();

			for (int j = 0; j < kLayerCount; ++j) {
				OvrLayer& layer = m_vrCtx->m_layers[j];
				layer.m_ovrLayer.Fov[i] = eyeDesc.Fov;
				layer.m_ovrLayer.RenderPose[i] = eyePose;
				layer.m_ovrLayer.SensorSampleTime = sampleTime;
			}
		}

		// build combined frustum
		m_frustumCombinedW = Frustum(m_eyeCameras[0].getWorldFrustum(), m_eyeCameras[1].getWorldFrustum());
	}
}

// PRIVATE

bool AppSampleVr::initVr()
{
	if (OVR_FAILURE(ovr_Initialize(0))) {
		APT_LOG_ERR("%s", OvrErrorString());
		APT_ASSERT(false);
		return false;
	}
	if (OVR_FAILURE(ovr_Create(&m_vrCtx->m_ovrSession, &m_vrCtx->m_ovrGraphicsLuid))) {
		APT_LOG_ERR("%s", OvrErrorString());
		if (OvrErrorResult() == ovrError_NoHmd) {
			m_vrMode = false;
			return true;
		}
		APT_ASSERT(false);
	}
	m_vrCtx->m_ovrHmdDesc = ovr_GetHmdDesc(m_vrCtx->m_ovrSession);
	if (m_vrCtx->m_ovrHmdDesc.Type != ovrHmd_CV1 && m_vrCtx->m_ovrHmdDesc.Type != ovrHmd_CB) {
		APT_LOG_ERR("Invalid HMD (only Oculus CV1 supported)");
		m_vrMode = false;
		return true;
		APT_ASSERT(false);
	}

	String<256> descStr;
	descStr.appendf("VR subsystem:      '%s'",     ovr_GetVersionString());
	descStr.appendf("HMD Info:");
	descStr.appendf("\n\tManufacturer:  '%s'",     m_vrCtx->m_ovrHmdDesc.Manufacturer);
	descStr.appendf("\n\tSerial Number:  %s",      m_vrCtx->m_ovrHmdDesc.SerialNumber);
	descStr.appendf("\n\tFirmware:       %d.%d",   m_vrCtx->m_ovrHmdDesc.FirmwareMajor, m_vrCtx->m_ovrHmdDesc.FirmwareMinor);
	descStr.appendf("\n\tResolution:     %dx%dpx", m_vrCtx->m_ovrHmdDesc.Resolution.w, m_vrCtx->m_ovrHmdDesc.Resolution.h);
	descStr.appendf("\n\tRefresh Rate:   %1.3fHz", m_vrCtx->m_ovrHmdDesc.DisplayRefreshRate);
	APT_LOG((const char*)descStr);

 // init layers = 1 swap chain per layer per eye
	for (int i = 0; i < kLayerCount; ++i) {
		OvrLayer& layer = m_vrCtx->m_layers[i];
		layer.m_ovrLayer.Header.Type = ovrLayerType_EyeFov;
		layer.m_ovrLayer.Header.Flags = ovrLayerFlag_TextureOriginAtBottomLeft; // OpenGL
		if (i == kLayerText) {
			layer.m_ovrLayer.Header.Flags |= ovrLayerFlag_HighQuality;
		}

		for (int j = 0; j < kEyeCount; ++j) {
			ovrSizei renderSize = ovr_GetFovTextureSize(m_vrCtx->m_ovrSession, (ovrEyeType)j, m_vrCtx->m_ovrHmdDesc.DefaultEyeFov[j], 1.0f);

		 // \todo per-sample settings for these?
		 // \todo ovr docs recommend creating power-of-two textures (for HQ layers) then using a smaller viewport
			ovrTextureSwapChainDesc swapDesc = {};
			swapDesc.StaticImage = ovrFalse;
			swapDesc.Width = renderSize.w;
			swapDesc.Height = renderSize.h;
			swapDesc.Type = ovrTexture_2D;
			swapDesc.ArraySize = 1;
			swapDesc.Format = OVR_FORMAT_R8G8B8A8_UNORM_SRGB;
			swapDesc.SampleCount = 1;
			swapDesc.MipLevels = 1;
			if (i == kLayerText) {
			 // allocate mip chain for the text layer
				swapDesc.MipLevels = APT_MIN(Texture::GetMaxMipCount(renderSize.w, renderSize.h), 8); 
			}

			if (OVR_FAILURE(ovr_CreateTextureSwapChainGL(m_vrCtx->m_ovrSession, &swapDesc, &layer.m_ovrSwapChain[j]))) {
				APT_LOG_ERR("%s", OvrErrorString());
				APT_ASSERT(false);
				return false;
			}

			ovrAssert(ovr_GetTextureSwapChainLength(m_vrCtx->m_ovrSession, layer.m_ovrSwapChain[j], &layer.m_swapChainLength));
			layer.m_chainTextures[j] = new Texture*[layer.m_swapChainLength];
			layer.m_chainFramebuffers[j] = new Framebuffer*[layer.m_swapChainLength];
			for (int k = 0; k < layer.m_swapChainLength; ++k) {
				GLuint txH;
				ovrAssert(ovr_GetTextureSwapChainBufferGL(m_vrCtx->m_ovrSession, layer.m_ovrSwapChain[j], k, &txH));
				String<32> nameStr("#VR_Chain%d_Layer%d_%s", k, i, (j == 0) ? "Left" : "Right");
				layer.m_chainTextures[j][k] = Texture::CreateProxy(txH, (const char*)nameStr);
				layer.m_chainFramebuffers[j][k] = Framebuffer::Create(1, layer.m_chainTextures[j][k]);
			}

			// by default use the entire texture
			layer.m_ovrLayer.ColorTexture[j] = layer.m_ovrSwapChain[j];
			layer.m_ovrLayer.Viewport[j].Pos.x = 0;
			layer.m_ovrLayer.Viewport[j].Pos.y = 0;
			layer.m_ovrLayer.Viewport[j].Size = renderSize;
		}
	}

	ovrSizei renderSize = ovr_GetFovTextureSize(m_vrCtx->m_ovrSession, (ovrEyeType)0, m_vrCtx->m_ovrHmdDesc.DefaultEyeFov[0], 1.0f);
	int mirrorDiv = 2; // divides render res
	ovrMirrorTextureDesc mirrorDesc = {};
	mirrorDesc.Format = OVR_FORMAT_R8G8B8A8_UNORM_SRGB;
	mirrorDesc.Width = (renderSize.w * 2) / mirrorDiv;
	mirrorDesc.Height = renderSize.h / mirrorDiv;
	mirrorDesc.MiscFlags = ovrTextureMisc_None;
	ovrAssert(ovr_CreateMirrorTextureGL(m_vrCtx->m_ovrSession, &mirrorDesc, &m_vrCtx->m_ovrMirrorTexture));
	GLuint mirrorH;
	ovrAssert(ovr_GetMirrorTextureBufferGL(m_vrCtx->m_ovrSession, m_vrCtx->m_ovrMirrorTexture, &mirrorH));
	m_txHmdMirror = Texture::CreateProxy(mirrorH, "#VR_Mirror");

	return true;
}

void AppSampleVr::shutdownVr()
{
	if (m_txHmdMirror) { 
		Texture::Destroy(m_txHmdMirror);
		ovr_DestroyMirrorTexture(m_vrCtx->m_ovrSession, m_vrCtx->m_ovrMirrorTexture);
	}

	for (int i = 0; i < kLayerCount; ++i) {
		OvrLayer& layer = m_vrCtx->m_layers[i];
		if (!layer.m_ovrSwapChain) {
			continue;
		}
		for (int j = 0; j < kEyeCount; ++j) {
			for (int k = 0; k < layer.m_swapChainLength; ++k) {
				Framebuffer::Destroy(layer.m_chainFramebuffers[j][k]);
				Texture::Destroy(layer.m_chainTextures[j][k]);
			}
			ovr_DestroyTextureSwapChain(m_vrCtx->m_ovrSession, layer.m_ovrSwapChain[j]);
		}
	}
	if (m_vrCtx->m_ovrSession) {
		ovr_Destroy(m_vrCtx->m_ovrSession);
	}
	ovr_Shutdown();
}

void AppSampleVr::drawVuiScreen(const Camera& _cam)
{
	GlContext* ctx = getGlContext();
	glAssert(glEnable(GL_BLEND));
	glAssert(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
	ctx->setShader  (m_shVuiScreen);
	ctx->bindTexture("txVuiScreen", m_txVuiScreen);
	ctx->setUniform ("uScale", vec2(*m_vuiScreenSize * *m_vuiScreenAspect, *m_vuiScreenSize));
	ctx->setUniform ("uWorldMatrix", m_vuiScreenWorldMatrix);
	ctx->setUniform ("uViewProjMatrix", _cam.getViewProjMatrix());
	drawNdcQuad();
	glAssert(glDisable(GL_BLEND));
}
