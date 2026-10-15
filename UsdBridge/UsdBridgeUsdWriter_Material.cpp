// Copyright 2020 The Khronos Group

#include "UsdBridgeUsdWriter.h"

#include "UsdBridgeUsdWriter_Common.h"
#include "UsdBridgeUsdWriter_Arrays.h"
#include "UsdBridgeDiffCapture.h"
#include "stb_image_write.h"

#include <limits>

_TF_TOKENS_STRUCT_NAME(QualifiedInputTokens)::_TF_TOKENS_STRUCT_NAME(QualifiedInputTokens)()
  : roughness(TfToken("inputs:roughness", TfToken::Immortal))
  , opacity(TfToken("inputs:opacity", TfToken::Immortal))
  , opacityThreshold(TfToken("inputs:opacityThreshold", TfToken::Immortal))
  , metallic(TfToken("inputs:metallic", TfToken::Immortal))
  , ior(TfToken("inputs:ior", TfToken::Immortal))
  , diffuseColor(TfToken("inputs:diffuseColor", TfToken::Immortal))
  , specularColor(TfToken("inputs:specularColor", TfToken::Immortal))
  , emissiveColor(TfToken("inputs:emissiveColor", TfToken::Immortal))
  , file(TfToken("inputs:file", TfToken::Immortal))
  , WrapS(TfToken("inputs:WrapS", TfToken::Immortal))
  , WrapT(TfToken("inputs:WrapT", TfToken::Immortal))
  , WrapR(TfToken("inputs:WrapR", TfToken::Immortal))
  , varname(TfToken("inputs:varname", TfToken::Immortal))

  , reflection_roughness_constant(TfToken("inputs:reflection_roughness_constant", TfToken::Immortal))
  , opacity_constant(TfToken("inputs:opacity_constant", TfToken::Immortal))
  , opacity_threshold(TfToken("inputs:opacity_threshold", TfToken::Immortal))
  , enable_opacity(TfToken("inputs:enable_opacity", TfToken::Immortal))
  , metallic_constant(TfToken("inputs:metallic_constant", TfToken::Immortal))
  , ior_constant(TfToken("inputs:ior_constant"))
  , diffuse_color_constant(TfToken("inputs:diffuse_color_constant", TfToken::Immortal))
  , emissive_color(TfToken("inputs:emissive_color", TfToken::Immortal))
  , emissive_intensity(TfToken("inputs:emissive_intensity", TfToken::Immortal))
  , enable_emission(TfToken("inputs:enable_emission", TfToken::Immortal))
  , name(TfToken("inputs:name", TfToken::Immortal))
  , tex(TfToken("inputs:tex", TfToken::Immortal))
  , wrap_u(TfToken("inputs:wrap_u", TfToken::Immortal))
  , wrap_v(TfToken("inputs:wrap_v", TfToken::Immortal))
  , wrap_w(TfToken("inputs:wrap_w", TfToken::Immortal))
  , a(TfToken("inputs:a", TfToken::Immortal))
  , b(TfToken("inputs:b", TfToken::Immortal))
{}
_TF_TOKENS_STRUCT_NAME(QualifiedInputTokens)::~_TF_TOKENS_STRUCT_NAME(QualifiedInputTokens)() = default;
TfStaticData<_TF_TOKENS_STRUCT_NAME(QualifiedInputTokens)> QualifiedInputTokens;

_TF_TOKENS_STRUCT_NAME(QualifiedOutputTokens)::_TF_TOKENS_STRUCT_NAME(QualifiedOutputTokens)()
  : result(TfToken("outputs:result", TfToken::Immortal))
  , r(TfToken("outputs:r", TfToken::Immortal))
  , rg(TfToken("outputs:rg", TfToken::Immortal))
  , rgb(TfToken("outputs:rgb", TfToken::Immortal))
  , a(TfToken("outputs:a", TfToken::Immortal))

  , out(TfToken("outputs:out", TfToken::Immortal))
{}
_TF_TOKENS_STRUCT_NAME(QualifiedOutputTokens)::~_TF_TOKENS_STRUCT_NAME(QualifiedOutputTokens)() = default;
TfStaticData<_TF_TOKENS_STRUCT_NAME(QualifiedOutputTokens)> QualifiedOutputTokens;

namespace
{
  struct StbWriteOutput
  {
    StbWriteOutput() = default;
    ~StbWriteOutput()
    {
      delete[] imageData;
    }

    char* imageData = nullptr;
    size_t imageSize = 0;
  };

  void StbWriteToBuffer(void *context, void *data, int size)
  {
    if(data)
    {
      StbWriteOutput* output = reinterpret_cast<StbWriteOutput*>(context);
      output->imageData = new char[size];
      output->imageSize = size;

      memcpy(output->imageData, data, size);
    }
  }

  template<typename CType>
  void ConvertSamplerData_Inner(const UsdBridgeSamplerData& samplerData, double normFactor, std::vector<unsigned char>& imageData)
  {
    int numComponents = samplerData.ImageNumComponents;
    uint64_t imageDimX = samplerData.ImageDims[0];
    uint64_t imageDimY = samplerData.ImageDims[1];
    int64_t yStride = samplerData.ImageStride[1];
    const char* samplerDataPtr = reinterpret_cast<const char*>(samplerData.Data);

    int64_t baseAddr = 0;
    for(uint64_t pY = 0; pY < imageDimY; ++pY, baseAddr += yStride)
    {
      const CType* lineAddr = reinterpret_cast<const CType*>(samplerDataPtr + baseAddr);
      for(uint64_t flatX = 0; flatX < imageDimX*numComponents; ++flatX) //flattened X index
      {
        uint64_t dstElt = numComponents*pY*imageDimX + flatX;

        double result = *(lineAddr+flatX)*normFactor;
        result = (result < 0.0) ? 0.0 : ((result > 255.0) ? 255.0 : result);

        imageData[dstElt] = static_cast<unsigned char>(result);
      }
    }
  }

  void ConvertSamplerDataToImage(const UsdBridgeSamplerData& samplerData, std::vector<unsigned char>& imageData)
  {
    UsdBridgeType flattenedType = ubutils::UsdBridgeTypeFlatten(samplerData.DataType);
    int numComponents = samplerData.ImageNumComponents;
    uint64_t imageDimX = samplerData.ImageDims[0];
    uint64_t imageDimY = samplerData.ImageDims[1];

    bool normalizedFp = (flattenedType == UsdBridgeType::FLOAT) || (flattenedType == UsdBridgeType::DOUBLE);
    bool fixedPoint = (flattenedType == UsdBridgeType::USHORT) || (flattenedType == UsdBridgeType::UINT);

    if(normalizedFp || fixedPoint)
    {
      imageData.resize(numComponents*imageDimX*imageDimY);

      switch(flattenedType)
      {
        case UsdBridgeType::FLOAT: ConvertSamplerData_Inner<float>(samplerData, 255.0, imageData); break;
        case UsdBridgeType::DOUBLE: ConvertSamplerData_Inner<double>(samplerData, 255.0, imageData); break;
        case UsdBridgeType::USHORT: ConvertSamplerData_Inner<unsigned short>(samplerData,
          255.0 / static_cast<double>(std::numeric_limits<unsigned short>::max()), imageData); break;
        case UsdBridgeType::UINT: ConvertSamplerData_Inner<unsigned int>(samplerData,
          255.0 / static_cast<double>(std::numeric_limits<unsigned int>::max()), imageData); break;
        default: break;
      }
    }
  }

  template<typename DataType>
  void CreateShaderInput(UsdShadeShader& shader, const TimeEvaluator<DataType>* timeEval, typename DataType::DataMemberId dataMemberId,
    const TfToken& inputToken, const TfToken& qualifiedInputToken, const SdfValueTypeName& valueType)
  {
    if(!timeEval || timeEval->IsTimeVarying(dataMemberId))
      shader.CreateInput(inputToken, valueType);
    else
      shader.GetPrim().RemoveProperty(qualifiedInputToken);
  }

  template<bool PreviewSurface>
  void CreateMaterialShaderInput(UsdShadeShader& shader, const TimeEvaluator<UsdBridgeMaterialData>* timeEval,
    typename UsdBridgeMaterialData::DataMemberId dataMemberId, const SdfValueTypeName& valueType)
  {
    const TfToken& inputToken = GetMaterialShaderInputToken<PreviewSurface>(dataMemberId);
    const TfToken& qualifiedInputToken = GetMaterialShaderInputQualifiedToken<PreviewSurface>(dataMemberId);
    CreateShaderInput(shader, timeEval, dataMemberId, inputToken, qualifiedInputToken, valueType);
  }

  template<typename ValueType, typename DataType>
  void SetShaderInput(UsdShadeShader& uniformShadPrim, UsdShadeShader& timeVarShadPrim, const TimeEvaluator<DataType>& timeEval,
    const TfToken& inputToken, typename DataType::DataMemberId dataMemberId, ValueType value)
  {
    using DMI = typename DataType::DataMemberId;

    bool timeVaryingUpdate = timeEval.IsTimeVarying(dataMemberId);

    UsdShadeInput timeVarInput, uniformInput;
    UsdAttribute uniformAttrib, timeVarAttrib;
    if(timeVarShadPrim) // Allow for non-existing prims (based on timeVaryingUpdate)
    {
      timeVarInput = timeVarShadPrim.GetInput(inputToken);
      assert(timeVarInput);
      timeVarAttrib = timeVarInput.GetAttr();
    }
    if(uniformShadPrim)
    {
      uniformInput = uniformShadPrim.GetInput(inputToken);
      assert(uniformInput);
      uniformAttrib = uniformInput.GetAttr();
    }

    ClearUsdAttributes(uniformAttrib, timeVarAttrib, timeVaryingUpdate);

    SET_TIMEVARYING_ATTRIB(timeVaryingUpdate, timeVarInput, uniformInput, value);
  }

  UsdShadeOutput InitializeMdlGraphNode(UsdShadeShader& graphNode, const TfToken& assetSubIdent, const SdfValueTypeName& returnType,
    const char* sourceAssetPath = constring::mdlSupportAssetName)
  {
    graphNode.CreateImplementationSourceAttr(VtValue(UsdBridgeTokens->sourceAsset));
    graphNode.SetSourceAsset(SdfAssetPath(sourceAssetPath), UsdBridgeTokens->mdl);
    graphNode.SetSourceAssetSubIdentifier(assetSubIdent, UsdBridgeTokens->mdl);
    return graphNode.CreateOutput(UsdBridgeTokens->out, returnType);
  }

  struct ShadeGraphTypeConversionNodeContext
  {
    ShadeGraphTypeConversionNodeContext(UsdStageRefPtr sceneStage,
      const SdfPath& matPrimPath,
      const char* connectionIdentifier)
      : SceneStage(sceneStage)
      , MatPrimPath(matPrimPath)
      , ConnectionIdentifier(connectionIdentifier)
    {
    }

    UsdShadeShader CreateAndConnectMdlTypeConversionNode(UsdShadeOutput& sourceOutput, const char* primNamePf, const TfToken& assetSubIdent,
      const SdfValueTypeName& inType, const SdfValueTypeName& outType, const char* sourceAssetPath = constring::mdlSupportAssetName) const
    {
      SdfPath nodePrimPath = this->MatPrimPath.AppendPath(SdfPath(std::string(this->ConnectionIdentifier)+primNamePf));
      UsdShadeShader conversionNode = UsdShadeShader::Get(this->SceneStage, nodePrimPath);
      UsdShadeOutput nodeOut;
      if(!conversionNode)
      {
        conversionNode = UsdShadeShader::Define(this->SceneStage, nodePrimPath);
        assert(conversionNode);
        nodeOut = InitializeMdlGraphNode(conversionNode, assetSubIdent, outType, sourceAssetPath);
        conversionNode.CreateInput(UsdBridgeTokens->a, inType);
      }
      else
        nodeOut = conversionNode.GetOutput(UsdBridgeTokens->out);

      conversionNode.GetInput(UsdBridgeTokens->a).ConnectToSource(sourceOutput);
      sourceOutput = nodeOut;

      return conversionNode;
    }

    void InsertMdlShaderTypeConversionNodes(const UsdShadeInput& shadeInput, UsdShadeOutput& nodeOutput) const
    {
      if(shadeInput.GetTypeName() == SdfValueTypeNames->Color3f)
      {
        if(nodeOutput.GetTypeName() == SdfValueTypeNames->Float4)
        {
          CreateAndConnectMdlTypeConversionNode(nodeOutput, constring::mdlGraphXYZPrimPf,
            UsdBridgeTokens->xyz, SdfValueTypeNames->Float4, SdfValueTypeNames->Float3, constring::mdlAuxAssetName);
        }
        if(nodeOutput.GetTypeName() == SdfValueTypeNames->Float3)
        {
          CreateAndConnectMdlTypeConversionNode(nodeOutput, constring::mdlGraphColorPrimPf,
            UsdBridgeTokens->construct_color, SdfValueTypeNames->Float3, SdfValueTypeNames->Color3f, constring::mdlAuxAssetName);
        }
      }
      if(shadeInput.GetTypeName() == SdfValueTypeNames->Float)
      {
        const char* componentPrimPf = constring::mdlGraphWPrimPf;
        TfToken componentAssetIdent = (nodeOutput.GetTypeName() == SdfValueTypeNames->Float4) ? UsdBridgeTokens->w : UsdBridgeTokens->x;
        switch(ChannelSelector)
        {
          case 0: componentPrimPf = constring::mdlGraphXPrimPf; componentAssetIdent = UsdBridgeTokens->x; break;
          case 1: componentPrimPf = constring::mdlGraphYPrimPf; componentAssetIdent = UsdBridgeTokens->y; break;
          case 2: componentPrimPf = constring::mdlGraphZPrimPf; componentAssetIdent = UsdBridgeTokens->z; break;
          default: break;
        }

        if(nodeOutput.GetTypeName() == SdfValueTypeNames->Float4)
        {
          CreateAndConnectMdlTypeConversionNode(nodeOutput, componentPrimPf,
            componentAssetIdent, SdfValueTypeNames->Float4, SdfValueTypeNames->Float, constring::mdlAuxAssetName);
        }
        else if(nodeOutput.GetTypeName() == SdfValueTypeNames->Float3)
        {
          CreateAndConnectMdlTypeConversionNode(nodeOutput, componentPrimPf,
            componentAssetIdent, SdfValueTypeNames->Float3, SdfValueTypeNames->Float, constring::mdlAuxAssetName);
        }
      }
    }

    UsdStageRefPtr SceneStage;
    const SdfPath& MatPrimPath;
    const char* ConnectionIdentifier = nullptr;

    int ChannelSelector = 0;
  };

  void InitializePsShaderUniform(UsdShadeShader& shader)
  {
    shader.CreateIdAttr(VtValue(UsdBridgeTokens->UsdPreviewSurface));
    shader.CreateInput(UsdBridgeTokens->useSpecularWorkflow, SdfValueTypeNames->Int).Set(0);
  }

  void InitializePsShaderTimeVar(UsdShadeShader& shader, const TimeEvaluator<UsdBridgeMaterialData>* timeEval = nullptr)
  {
    typedef UsdBridgeMaterialData::DataMemberId DMI;

    CreateMaterialShaderInput<true>(shader, timeEval, DMI::DIFFUSE, SdfValueTypeNames->Color3f);
    CreateMaterialShaderInput<true>(shader, timeEval, DMI::EMISSIVECOLOR, SdfValueTypeNames->Color3f);
    CreateMaterialShaderInput<true>(shader, timeEval, DMI::ROUGHNESS, SdfValueTypeNames->Float);
    CreateMaterialShaderInput<true>(shader, timeEval, DMI::OPACITY, SdfValueTypeNames->Float);
    CreateMaterialShaderInput<true>(shader, timeEval, DMI::METALLIC, SdfValueTypeNames->Float);
    CreateMaterialShaderInput<true>(shader, timeEval, DMI::IOR, SdfValueTypeNames->Float);

    CreateShaderInput(shader, timeEval, DMI::OPACITY, UsdBridgeTokens->opacityThreshold, QualifiedInputTokens->opacityThreshold, SdfValueTypeNames->Float);
  }

  void InitializeMdlShaderUniform(UsdShadeShader& shader)
  {
    typedef UsdBridgeMaterialData::DataMemberId DMI;

    shader.CreateImplementationSourceAttr(VtValue(UsdBridgeTokens->sourceAsset));
    shader.SetSourceAsset(SdfAssetPath(constring::mdlShaderAssetName), UsdBridgeTokens->mdl);
    shader.SetSourceAssetSubIdentifier(UsdBridgeTokens->OmniPBR, UsdBridgeTokens->mdl);

    shader.CreateInput(GetMaterialShaderInputToken<false>(DMI::OPACITY), SdfValueTypeNames->Float); // Opacity is handled by the opacitymul node
  }

  void InitializeMdlShaderTimeVar(UsdShadeShader& shader, const TimeEvaluator<UsdBridgeMaterialData>* timeEval = nullptr)
  {
    typedef UsdBridgeMaterialData::DataMemberId DMI;

    CreateMaterialShaderInput<false>(shader, timeEval, DMI::DIFFUSE, SdfValueTypeNames->Color3f);
    CreateMaterialShaderInput<false>(shader, timeEval, DMI::EMISSIVECOLOR, SdfValueTypeNames->Color3f);
    CreateMaterialShaderInput<false>(shader, timeEval, DMI::EMISSIVEINTENSITY, SdfValueTypeNames->Float);
    CreateMaterialShaderInput<false>(shader, timeEval, DMI::ROUGHNESS, SdfValueTypeNames->Float);
    CreateMaterialShaderInput<false>(shader, timeEval, DMI::METALLIC, SdfValueTypeNames->Float);

    CreateShaderInput(shader, timeEval, DMI::OPACITY, UsdBridgeTokens->enable_opacity, QualifiedInputTokens->enable_opacity, SdfValueTypeNames->Bool);
    CreateShaderInput(shader, timeEval, DMI::OPACITY, UsdBridgeTokens->opacity_threshold, QualifiedInputTokens->opacity_threshold, SdfValueTypeNames->Float);
    CreateShaderInput(shader, timeEval, DMI::EMISSIVEINTENSITY, UsdBridgeTokens->enable_emission, QualifiedInputTokens->enable_emission, SdfValueTypeNames->Bool);
  }

  UsdShadeOutput InitializePsAttributeReaderUniform(UsdShadeShader& attributeReader, const TfToken& readerId = TfToken(), const SdfValueTypeName& returnType = SdfValueTypeName())
  {
    if (!readerId.IsEmpty())
      attributeReader.CreateIdAttr(VtValue(readerId));
    else
      attributeReader.CreateIdAttr(VtValue(UsdBridgeTokens->PrimVarReader_Float));

    attributeReader.CreateInput(UsdBridgeTokens->varname, SdfValueTypeNames->Token);

    if (returnType != SdfValueTypeName())
      return attributeReader.CreateOutput(UsdBridgeTokens->result, returnType);
    else
      return attributeReader.CreateOutput(UsdBridgeTokens->result, SdfValueTypeNames->Float);
  }

  template<typename DataType>
  void InitializePsAttributeReaderTimeVar(UsdShadeShader& attributeReader, typename DataType::DataMemberId dataMemberId, const TimeEvaluator<DataType>* timeEval)
  {
  }

  UsdShadeOutput InitializeMdlAttributeReaderUniform(UsdShadeShader& attributeReader, const TfToken& readerId = TfToken(), const SdfValueTypeName& returnType = SdfValueTypeName())
  {
    attributeReader.CreateImplementationSourceAttr(VtValue(UsdBridgeTokens->sourceAsset));
    attributeReader.SetSourceAsset(SdfAssetPath(constring::mdlSupportAssetName), UsdBridgeTokens->mdl);

    if (!readerId.IsEmpty())
      attributeReader.SetSourceAssetSubIdentifier(readerId, UsdBridgeTokens->mdl);
    else
      attributeReader.SetSourceAssetSubIdentifier(UsdBridgeTokens->data_lookup_float, UsdBridgeTokens->mdl);

    attributeReader.CreateInput(UsdBridgeTokens->name, SdfValueTypeNames->String);

    if (returnType != SdfValueTypeName())
      return attributeReader.CreateOutput(UsdBridgeTokens->out, returnType);
    else
      return attributeReader.CreateOutput(UsdBridgeTokens->out, SdfValueTypeNames->Float);
  }

  template<typename DataType>
  void InitializeMdlAttributeReaderTimeVar(UsdShadeShader& attributeReader, typename DataType::DataMemberId dataMemberId, const TimeEvaluator<DataType>* timeEval)
  {
  }

  void InitializePsSamplerUniform(UsdShadeShader& sampler, const SdfValueTypeName& coordType, const UsdShadeOutput& tcrOutput)
  {
    sampler.CreateIdAttr(VtValue(UsdBridgeTokens->UsdUVTexture));

    sampler.CreateOutput(UsdBridgeTokens->rgb, SdfValueTypeNames->Float3);
    sampler.CreateOutput(UsdBridgeTokens->a, SdfValueTypeNames->Float);

    sampler.CreateInput(UsdBridgeTokens->fallback, SdfValueTypeNames->Float4).Set(GfVec4f(1.0f, 0.0f, 0.0f, 1.0f));
    sampler.CreateInput(UsdBridgeTokens->st, coordType).ConnectToSource(tcrOutput);
  }

  void InitializePsSamplerTimeVar(UsdShadeShader& sampler, UsdBridgeSamplerData::SamplerType type, const TimeEvaluator<UsdBridgeSamplerData>* timeEval = nullptr)
  {
    typedef UsdBridgeSamplerData::DataMemberId DMI;

    CreateShaderInput(sampler, timeEval, DMI::DATA, UsdBridgeTokens->file, QualifiedInputTokens->file, SdfValueTypeNames->Asset);
    CreateShaderInput(sampler, timeEval, DMI::WRAPS, UsdBridgeTokens->WrapS, QualifiedInputTokens->WrapS, SdfValueTypeNames->Token);
    if((uint32_t)type >= (uint32_t)UsdBridgeSamplerData::SamplerType::SAMPLER_2D)
      CreateShaderInput(sampler, timeEval, DMI::WRAPT, UsdBridgeTokens->WrapT, QualifiedInputTokens->WrapT, SdfValueTypeNames->Token);
    if((uint32_t)type >= (uint32_t)UsdBridgeSamplerData::SamplerType::SAMPLER_3D)
      CreateShaderInput(sampler, timeEval, DMI::WRAPR, UsdBridgeTokens->WrapR, QualifiedInputTokens->WrapR, SdfValueTypeNames->Token);
  }

  void InitializeMdlSamplerUniform(UsdShadeShader& sampler, const SdfValueTypeName& coordType, const UsdShadeOutput& tcrOutput)
  {
    sampler.CreateImplementationSourceAttr(VtValue(UsdBridgeTokens->sourceAsset));
    sampler.SetSourceAsset(SdfAssetPath(constring::mdlSupportAssetName), UsdBridgeTokens->mdl);
    sampler.SetSourceAssetSubIdentifier(UsdBridgeTokens->lookup_float4, UsdBridgeTokens->mdl);

    sampler.CreateOutput(UsdBridgeTokens->out, SdfValueTypeNames->Float4);

    sampler.CreateInput(UsdBridgeTokens->coord, coordType).ConnectToSource(tcrOutput);
  }

  void InitializeMdlSamplerTimeVar(UsdShadeShader& sampler, UsdBridgeSamplerData::SamplerType type, const TimeEvaluator<UsdBridgeSamplerData>* timeEval = nullptr)
  {
    typedef UsdBridgeSamplerData::DataMemberId DMI;

    CreateShaderInput(sampler, timeEval, DMI::DATA, UsdBridgeTokens->tex, QualifiedInputTokens->tex, SdfValueTypeNames->Asset);
    CreateShaderInput(sampler, timeEval, DMI::WRAPS, UsdBridgeTokens->wrap_u, QualifiedInputTokens->wrap_u, SdfValueTypeNames->Int);
    if((uint32_t)type >= (uint32_t)UsdBridgeSamplerData::SamplerType::SAMPLER_2D)
      CreateShaderInput(sampler, timeEval, DMI::WRAPT, UsdBridgeTokens->wrap_v, QualifiedInputTokens->wrap_v, SdfValueTypeNames->Int);
    if((uint32_t)type >= (uint32_t)UsdBridgeSamplerData::SamplerType::SAMPLER_3D)
      CreateShaderInput(sampler, timeEval, DMI::WRAPR, UsdBridgeTokens->wrap_w, QualifiedInputTokens->wrap_w, SdfValueTypeNames->Int);
  }

  UsdShadeOutput InitializePsShader(UsdStageRefPtr shaderStage, const SdfPath& matPrimPath, bool uniformPrim
    , const TimeEvaluator<UsdBridgeMaterialData>* timeEval = nullptr)
  {
    SdfPath shadPrimPath = matPrimPath.AppendPath(SdfPath(constring::psShaderPrimPf));
    UsdShadeShader shader = GetOrDefinePrim<UsdShadeShader>(shaderStage, shadPrimPath);
    assert(shader);

    if (uniformPrim)
    {
      InitializePsShaderUniform(shader);
    }

    InitializePsShaderTimeVar(shader, timeEval);

    if(uniformPrim)
      return shader.CreateOutput(UsdBridgeTokens->surface, SdfValueTypeNames->Token);
    else
      return UsdShadeOutput();
  }

  UsdShadeOutput InitializeMdlShader(UsdStageRefPtr shaderStage, const SdfPath& matPrimPath, bool uniformPrim
    , const TimeEvaluator<UsdBridgeMaterialData>* timeEval = nullptr)
  {
    typedef UsdBridgeMaterialData::DataMemberId DMI;

    SdfPath shadPrimPath = matPrimPath.AppendPath(SdfPath(constring::mdlShaderPrimPf));
    UsdShadeShader shader = GetOrDefinePrim<UsdShadeShader>(shaderStage, shadPrimPath);
    assert(shader);

    SdfPath shadPrimPath_opmul = matPrimPath.AppendPath(SdfPath(constring::mdlOpacityMulPrimPf));
    UsdShadeShader opacityMul = GetOrDefinePrim<UsdShadeShader>(shaderStage, shadPrimPath_opmul);
    assert(opacityMul);
    UsdShadeOutput opacityMulOut;

    if(uniformPrim)
    {
      opacityMulOut = InitializeMdlGraphNode(opacityMul, UsdBridgeTokens->mul_float, SdfValueTypeNames->Float);
      opacityMul.CreateInput(UsdBridgeTokens->a, SdfValueTypeNames->Float);

      InitializeMdlShaderUniform(shader);
    }

    CreateShaderInput(opacityMul, timeEval, DMI::OPACITY, UsdBridgeTokens->b, QualifiedInputTokens->b, SdfValueTypeNames->Float);
    InitializeMdlShaderTimeVar(shader, timeEval);

    if(uniformPrim)
    {
      shader.GetInput(GetMaterialShaderInputToken<false>(DMI::OPACITY)).ConnectToSource(opacityMulOut);
    }

    if(uniformPrim)
      return shader.CreateOutput(UsdBridgeTokens->out, SdfValueTypeNames->Token);
    else
      return UsdShadeOutput();
  }

  void BindShaderToMaterial(const UsdShadeMaterial& matPrim, const UsdShadeOutput& shadOut, TfToken* renderContext)
  {
    if(renderContext)
      matPrim.CreateSurfaceOutput(*renderContext).ConnectToSource(shadOut);
    else
      matPrim.CreateSurfaceOutput().ConnectToSource(shadOut);
  }

  UsdShadeMaterial InitializeUsdMaterial_Impl(UsdStageRefPtr materialStage, const SdfPath& matPrimPath, bool uniformPrim, const UsdBridgeSettings& settings,
    const TimeEvaluator<UsdBridgeMaterialData>* timeEval = nullptr)
  {
    UsdShadeMaterial matPrim = GetOrDefinePrim<UsdShadeMaterial>(materialStage, matPrimPath);
    assert(matPrim);

    if(settings.EnablePreviewSurfaceShader)
    {
      UsdShadeOutput shaderOutput = InitializePsShader(materialStage, matPrimPath, uniformPrim, timeEval);

      if(uniformPrim)
        BindShaderToMaterial(matPrim, shaderOutput, nullptr);
    }

    if(settings.EnableMdlShader)
    {
      UsdShadeOutput shaderOutput = InitializeMdlShader(materialStage, matPrimPath, uniformPrim, timeEval);

      if(uniformPrim)
        BindShaderToMaterial(matPrim, shaderOutput, &UsdBridgeTokens->mdl);
    }

    return matPrim;
  }

  UsdPrim InitializePsSampler_Impl(UsdStageRefPtr samplerStage, const SdfPath& samplerPrimPath, UsdBridgeSamplerData::SamplerType type, bool uniformPrim,
    const TimeEvaluator<UsdBridgeSamplerData>* timeEval = nullptr)
  {
    typedef UsdBridgeSamplerData::DataMemberId DMI;

    UsdShadeShader sampler = GetOrDefinePrim<UsdShadeShader>(samplerStage, samplerPrimPath);
    assert(sampler);

    if(uniformPrim)
    {
      SdfPath texCoordReaderPrimPath = samplerPrimPath.AppendPath(SdfPath(constring::texCoordReaderPrimPf));
      UsdShadeShader texCoordReader = UsdShadeShader::Define(samplerStage, texCoordReaderPrimPath);
      assert(texCoordReader);

      TfToken idAttrib;
      SdfValueTypeName valueType;
      if(type == UsdBridgeSamplerData::SamplerType::SAMPLER_1D)
      {
        idAttrib = UsdBridgeTokens->PrimVarReader_Float;
        valueType = SdfValueTypeNames->Float;
      }
      else if (type == UsdBridgeSamplerData::SamplerType::SAMPLER_2D)
      {
        idAttrib = UsdBridgeTokens->PrimVarReader_Float2;
        valueType = SdfValueTypeNames->Float2;
      }
      else
      {
        idAttrib = UsdBridgeTokens->PrimVarReader_Float3;
        valueType = SdfValueTypeNames->Float3;
      }

      UsdShadeOutput tcrOutput = InitializePsAttributeReaderUniform(texCoordReader, idAttrib, valueType);

      InitializePsSamplerUniform(sampler, valueType, tcrOutput);
    }

    InitializePsSamplerTimeVar(sampler, type, timeEval);

    return sampler.GetPrim();
  }

  UsdPrim InitializeMdlSampler_Impl(UsdStageRefPtr samplerStage, const SdfPath& samplerPrimPath, UsdBridgeSamplerData::SamplerType type, bool uniformPrim,
    const TimeEvaluator<UsdBridgeSamplerData>* timeEval = nullptr)
  {
    typedef UsdBridgeSamplerData::DataMemberId DMI;

    UsdShadeShader sampler = GetOrDefinePrim<UsdShadeShader>(samplerStage, samplerPrimPath);
    assert(sampler);

    if(uniformPrim)
    {
      SdfPath texCoordReaderPrimPath = samplerPrimPath.AppendPath(SdfPath(constring::texCoordReaderPrimPf));
      UsdShadeShader texCoordReader = UsdShadeShader::Define(samplerStage, texCoordReaderPrimPath);
      assert(texCoordReader);

      TfToken idAttrib;
      SdfValueTypeName valueType;
      if(type == UsdBridgeSamplerData::SamplerType::SAMPLER_1D)
      {
        idAttrib = UsdBridgeTokens->data_lookup_float2; // Currently, there is no 1D lookup
        valueType = SdfValueTypeNames->Float2;
      }
      else if (type == UsdBridgeSamplerData::SamplerType::SAMPLER_2D)
      {
        idAttrib = UsdBridgeTokens->data_lookup_float2;
        valueType = SdfValueTypeNames->Float2;
      }
      else
      {
        idAttrib = UsdBridgeTokens->data_lookup_float3;
        valueType = SdfValueTypeNames->Float3;
      }

      UsdShadeOutput tcrOutput = InitializeMdlAttributeReaderUniform(texCoordReader, idAttrib, valueType);

      InitializeMdlSamplerUniform(sampler, valueType, tcrOutput);
    }

    InitializeMdlSamplerTimeVar(sampler, type, timeEval);

    if(uniformPrim)
    {
      sampler.GetInput(UsdBridgeTokens->tex).GetAttr().SetMetadata(UsdBridgeTokens->colorSpace, VtValue("sRGB"));
    }

    return sampler.GetPrim();
  }

  void InitializeSampler_Impl(UsdStageRefPtr samplerStage, const SdfPath& samplerPrimPath, UsdBridgeSamplerData::SamplerType type, bool uniformPrim,
    const UsdBridgeSettings& settings, const TimeEvaluator<UsdBridgeSamplerData>* timeEval = nullptr)
  {
    if(settings.EnablePreviewSurfaceShader)
    {
      SdfPath usdSamplerPrimPath = samplerPrimPath.AppendPath(SdfPath(constring::psSamplerPrimPf));
      InitializePsSampler_Impl(samplerStage, usdSamplerPrimPath, type, uniformPrim, timeEval);
    }

    if(settings.EnableMdlShader)
    {
      SdfPath usdSamplerPrimPath = samplerPrimPath.AppendPath(SdfPath(constring::mdlSamplerPrimPf));
      InitializeMdlSampler_Impl(samplerStage, usdSamplerPrimPath, type, uniformPrim, timeEval);
    }
  }

  template<bool PreviewSurface>
  UsdShadeShader InitializeAttributeReader_Impl(UsdStageRefPtr materialStage, const SdfPath& matPrimPath, bool uniformPrim,
    UsdBridgeMaterialData::DataMemberId dataMemberId, const TimeEvaluator<UsdBridgeMaterialData>* timeEval, const SdfPath& attribReaderPath = SdfPath())
  {
    using DMI = UsdBridgeMaterialData::DataMemberId;

    if(!uniformPrim) // timevar attribute reader disabled
      return UsdShadeShader();

    SdfPath attributeReaderPath = matPrimPath.AppendPath(
      (dataMemberId == DMI::NONE) ? attribReaderPath : GetAttributeReaderPathPf<PreviewSurface>(dataMemberId));
    UsdShadeShader attributeReader = UsdShadeShader::Get(materialStage, attributeReaderPath);

    if(!attributeReader || timeEval)
    {
      if(!attributeReader)
        attributeReader = UsdShadeShader::Define(materialStage, attributeReaderPath);

      if(PreviewSurface)
      {
        if(uniformPrim) // Implies !timeEval, so initialize
        {
          InitializePsAttributeReaderUniform(attributeReader);
        }

      }
      else
      {
        if(uniformPrim) // Implies !timeEval, so initialize
        {
          InitializeMdlAttributeReaderUniform(attributeReader);
        }

      }
    }

    return attributeReader;
  }

  #define INITIALIZE_ATTRIBUTE_READER_MACRO(srcAttrib, dmi) \
    if(srcAttrib) InitializeAttributeReader_Impl<PreviewSurface>(materialStage, matPrimPath, false, dmi, timeEval);

  template<bool PreviewSurface>
  void InitializeAttributeReaderSet(UsdStageRefPtr materialStage, const SdfPath& matPrimPath, const UsdBridgeSettings& settings,
    const UsdBridgeMaterialData& matData, const TimeEvaluator<UsdBridgeMaterialData>* timeEval)
  {
    using DMI = UsdBridgeMaterialData::DataMemberId;

    INITIALIZE_ATTRIBUTE_READER_MACRO(matData.Diffuse.SrcAttrib, DMI::DIFFUSE); \
    INITIALIZE_ATTRIBUTE_READER_MACRO(matData.Opacity.SrcAttrib, DMI::OPACITY); \
    INITIALIZE_ATTRIBUTE_READER_MACRO(matData.Emissive.SrcAttrib, DMI::EMISSIVECOLOR); \
    INITIALIZE_ATTRIBUTE_READER_MACRO(matData.EmissiveIntensity.SrcAttrib, DMI::EMISSIVEINTENSITY); \
    INITIALIZE_ATTRIBUTE_READER_MACRO(matData.Roughness.SrcAttrib, DMI::ROUGHNESS);
    INITIALIZE_ATTRIBUTE_READER_MACRO(matData.Metallic.SrcAttrib, DMI::METALLIC);
    INITIALIZE_ATTRIBUTE_READER_MACRO(matData.Ior.SrcAttrib, DMI::IOR);
  }

  void InitializeAttributeReaders_Impl(UsdStageRefPtr materialStage, const SdfPath& matPrimPath, const UsdBridgeSettings& settings,
    const UsdBridgeMaterialData& matData, const TimeEvaluator<UsdBridgeMaterialData>* timeEval)
  {

    if(settings.EnablePreviewSurfaceShader)
    {
      InitializeAttributeReaderSet<true>(materialStage, matPrimPath, settings, matData, timeEval);
    }

    if(settings.EnableMdlShader)
    {
      InitializeAttributeReaderSet<false>(materialStage, matPrimPath, settings, matData, timeEval);
    }
  }

  template<bool PreviewSurface>
  void GetOrCreateAttributeReaders(UsdStageRefPtr sceneStage, UsdStageRefPtr timeVarStage, const SdfPath& matPrimPath, UsdBridgeMaterialData::DataMemberId dataMemberId,
    UsdShadeShader& uniformReaderPrim, const SdfPath& attribReaderPath = SdfPath())
  {
    uniformReaderPrim = InitializeAttributeReader_Impl<PreviewSurface>(sceneStage, matPrimPath, true, dataMemberId, nullptr, attribReaderPath);
  }

  template<bool PreviewSurface>
  void UpdateAttributeReaderName(UsdShadeShader& uniformReaderPrim, const TfToken& nameToken)
  {
    if(PreviewSurface)
    {
      uniformReaderPrim.GetInput(UsdBridgeTokens->varname).Set(nameToken);
    }
    else
    {
      uniformReaderPrim.GetInput(UsdBridgeTokens->name).Set(nameToken.GetString());
    }
  }

  template<bool PreviewSurface>
  void UpdateAttributeReaderOutput(UsdShadeShader& uniformReaderPrim, const UsdGeomPrimvarsAPI& boundGeomPrimvars, const TfToken& attribNameToken)
  {
    const TfToken& outAttribToken = PreviewSurface ? UsdBridgeTokens->result : UsdBridgeTokens->out;
    UsdGeomPrimvar geomPrimvar = boundGeomPrimvars.GetPrimvar(attribNameToken);
    UsdShadeOutput readerOutput = uniformReaderPrim.GetOutput(outAttribToken);

    if(geomPrimvar)
    {
      const SdfValueTypeName& geomPrimTypeName = geomPrimvar.GetTypeName().GetScalarType();
      if(geomPrimTypeName != readerOutput.GetTypeName())
      {
        uniformReaderPrim.GetPrim().RemoveProperty(PreviewSurface ? QualifiedOutputTokens->result : QualifiedOutputTokens->out);
        uniformReaderPrim.CreateOutput(outAttribToken,
          (geomPrimTypeName == SdfValueTypeNames->Color3f) ? SdfValueTypeNames->Float3 : geomPrimTypeName);
        if(PreviewSurface)
        {
          uniformReaderPrim.CreateIdAttr(VtValue(GetPsAttributeReaderSubId(geomPrimTypeName)));
        }
        else
        {
          uniformReaderPrim.SetSourceAssetSubIdentifier(GetMdlAttributeReaderSubId(geomPrimTypeName), UsdBridgeTokens->mdl);
        }
      }
    }
  }

  template<bool PreviewSurface>
  void UpdateShaderInput_ShadeNode( const UsdShadeShader& uniformShaderIn, const UsdShadeShader& timeVarShaderIn, const TfToken& inputToken,
    const UsdShadeShader& shadeNodeOut, const TfToken& outputToken,
    const ShadeGraphTypeConversionNodeContext& conversionContext)
  {
    UsdShadeInput timeVarInput = timeVarShaderIn.GetInput(inputToken);
    if(timeVarInput)
      timeVarInput.GetAttr().Clear();

    UsdShadeOutput nodeOutput = shadeNodeOut.GetOutput(outputToken);
    assert(nodeOutput);

    UsdShadeInput shadeInput = uniformShaderIn.GetInput(inputToken);

    if(!PreviewSurface)
    {
      conversionContext.InsertMdlShaderTypeConversionNodes(shadeInput, nodeOutput);
    }

    uniformShaderIn.GetInput(inputToken).ConnectToSource(nodeOutput);
  }

  template<bool PreviewSurface>
  void UpdateShaderInputColorOpacity_Constant(UsdStageRefPtr sceneStage, const SdfPath& matPrimPath)
  {
    using DMI = UsdBridgeMaterialData::DataMemberId;

    if(PreviewSurface)
    {
      SdfPath shadPrimPath = matPrimPath.AppendPath(SdfPath(constring::psShaderPrimPf));
      UsdShadeShader uniformShadPrim = UsdShadeShader::Get(sceneStage, shadPrimPath);
      assert(uniformShadPrim);

      const TfToken& inputToken = GetMaterialShaderInputToken<true>(DMI::OPACITY);
      UsdShadeInput uniformShadInput = uniformShadPrim.GetInput(inputToken);
      assert(uniformShadInput);
      uniformShadInput.Set(1.0f);
    }
    else
    {
      SdfPath opMulPrimPath = matPrimPath.AppendPath(SdfPath(constring::mdlOpacityMulPrimPf));
      UsdShadeShader uniformOpMul = UsdShadeShader::Get(sceneStage, opMulPrimPath);

      UsdShadeInput uniformOpMulInput = uniformOpMul.GetInput(UsdBridgeTokens->a);
      assert(uniformOpMulInput);
      uniformOpMulInput.Set(1.0f);
    }
  }

  template<bool PreviewSurface>
  void UpdateShaderInputColorOpacity_AttributeReader(UsdStageRefPtr materialStage,
    const UsdShadeShader& outputNode, const ShadeGraphTypeConversionNodeContext& conversionContext)
  {
    using DMI = UsdBridgeMaterialData::DataMemberId;

    UsdShadeShader uniformShadPrim;
    UsdShadeShader timeVarShadPrim;
    TfToken inputToken;
    TfToken outputToken;

    if(PreviewSurface)
    {

      SdfPath shadPrimPath = conversionContext.MatPrimPath.AppendPath(SdfPath(constring::psShaderPrimPf));
      uniformShadPrim = UsdShadeShader::Get(conversionContext.SceneStage, shadPrimPath);
      assert(uniformShadPrim);
      timeVarShadPrim = UsdShadeShader::Get(materialStage, shadPrimPath);
      assert(timeVarShadPrim);

      inputToken = GetMaterialShaderInputToken<true>(DMI::OPACITY);
      outputToken = UsdBridgeTokens->result;
    }
    else
    {
      SdfPath opMulPrimPath = conversionContext.MatPrimPath.AppendPath(SdfPath(constring::mdlOpacityMulPrimPf));
      uniformShadPrim = UsdShadeShader::Get(conversionContext.SceneStage, opMulPrimPath);
      timeVarShadPrim = UsdShadeShader::Get(materialStage, opMulPrimPath);

      inputToken = UsdBridgeTokens->a;
      outputToken = UsdBridgeTokens->out;
    }
    UpdateShaderInput_ShadeNode<PreviewSurface>(uniformShadPrim, timeVarShadPrim, inputToken,
      outputNode, outputToken, conversionContext);
  }

  template<bool PreviewSurface>
  void UpdateShaderIndirectInput(UsdStageRefPtr sceneStage, UsdStageRefPtr timeVarStage,
    const SdfPath& matPrimPath, const UsdGeomPrimvarsAPI& boundGeomPrimvars, const TfToken& attribNameToken,
    UsdShadeShader& uniformReaderPrim, UsdBridgeMaterialData::DataMemberId dataMemberId, UsdBridgeSettings& settings)
  {
    using DMI = UsdBridgeMaterialData::DataMemberId;

    if(dataMemberId == DMI::DIFFUSE)
    {
      UsdGeomPrimvar geomPrimvar = boundGeomPrimvars.GetPrimvar(attribNameToken);
      if(geomPrimvar)
      {
        if (!PreviewSurface && geomPrimvar.GetTypeName().GetScalarType().GetDimensions() == 4)
        {
          ShadeGraphTypeConversionNodeContext conversionContext(
            sceneStage, matPrimPath, PreviewSurface ? "" : constring::mdlDiffuseOpacityPrimPf);
          conversionContext.ChannelSelector = 3; // Select the w channel from the float4 input

          UpdateShaderInputColorOpacity_AttributeReader<PreviewSurface>(timeVarStage, uniformReaderPrim, conversionContext);
        }
        else
        {
          UsdGeomPrimvar displayOpacityPrimvar;
          if(settings.UseDisplayColorOpacity)
            displayOpacityPrimvar = boundGeomPrimvars.GetPrimvar(UsdBridgeTokens->displayOpacity);

          if(settings.UseDisplayColorOpacity && attribNameToken == UsdBridgeTokens->displayColor && displayOpacityPrimvar)
          {
            SdfPath displayOpacityReaderPath = GetDisplayOpacityReaderPathPf<PreviewSurface>();

            UsdShadeShader displayOpacityReaderPrim;
            GetOrCreateAttributeReaders<PreviewSurface>(sceneStage, timeVarStage, matPrimPath,
              DMI::NONE, displayOpacityReaderPrim, displayOpacityReaderPath);

            UpdateAttributeReaderName<PreviewSurface>(displayOpacityReaderPrim, UsdBridgeTokens->displayOpacity);
            UpdateAttributeReaderOutput<PreviewSurface>(displayOpacityReaderPrim, boundGeomPrimvars, UsdBridgeTokens->displayOpacity);

            ShadeGraphTypeConversionNodeContext conversionContext(
              sceneStage, matPrimPath, PreviewSurface ? "" : constring::mdlDiffuseOpacityPrimPf);

            UpdateShaderInputColorOpacity_AttributeReader<PreviewSurface>(timeVarStage, displayOpacityReaderPrim, conversionContext);
          }
          else
          {
            UpdateShaderInputColorOpacity_Constant<PreviewSurface>(sceneStage, matPrimPath);
          }
        }
      }
    }
  }

  template<bool PreviewSurface, typename InputValueType, typename ValueType>
  void UpdateShaderInput(UsdBridgeUsdWriter* writer, UsdStageRefPtr sceneStage, UsdStageRefPtr timeVarStage,
    UsdShadeShader& uniformShadPrim, UsdShadeShader& timeVarShadPrim, const SdfPath& matPrimPath, const UsdGeomPrimvarsAPI& boundGeomPrimvars,
    const TimeEvaluator<UsdBridgeMaterialData>& timeEval, UsdBridgeMaterialData::DataMemberId dataMemberId,
    const UsdBridgeMaterialData::MaterialInput<InputValueType>& param, const TfToken& inputToken, const ValueType& inputValue)
  {
    using DMI = UsdBridgeMaterialData::DataMemberId;

    if (param.SrcAttrib != nullptr)
    {

      UsdShadeShader uniformReaderPrim;
      GetOrCreateAttributeReaders<PreviewSurface>(sceneStage,
        sceneStage, matPrimPath, dataMemberId, uniformReaderPrim);
      assert(uniformReaderPrim);

      const TfToken& attribNameToken = writer->AttributeNameToken(param.SrcAttrib);
      UpdateAttributeReaderName<PreviewSurface>(uniformReaderPrim, writer->AttributeNameToken(param.SrcAttrib));
      UpdateAttributeReaderOutput<PreviewSurface>(uniformReaderPrim, boundGeomPrimvars, attribNameToken);

      ShadeGraphTypeConversionNodeContext conversionContext(
        sceneStage, matPrimPath, GetAttributeReaderPathPf<PreviewSurface>(dataMemberId).GetString().c_str());

      const TfToken& outputToken = PreviewSurface ? UsdBridgeTokens->result : UsdBridgeTokens->out;
      UpdateShaderInput_ShadeNode<PreviewSurface>(uniformShadPrim, timeVarShadPrim, inputToken, uniformReaderPrim, outputToken, conversionContext);

      UpdateShaderIndirectInput<PreviewSurface>(sceneStage, timeVarStage,
        matPrimPath, boundGeomPrimvars, attribNameToken, uniformReaderPrim, dataMemberId, writer->Settings);
    }
    else if(!param.Sampler)
    {
      UsdShadeInput uniformDiffInput = uniformShadPrim.GetInput(inputToken);
      if(uniformDiffInput.HasConnectedSource())
        uniformDiffInput.DisconnectSource();

      SetShaderInput(uniformShadPrim, timeVarShadPrim, timeEval, inputToken, dataMemberId, inputValue);

      // The diffuse input owns the opacitymul's 'a' input.

      if(dataMemberId == DMI::DIFFUSE)
        UpdateShaderInputColorOpacity_Constant<PreviewSurface>(sceneStage, matPrimPath);
    }
  }

  template<bool PreviewSurface, typename InputValueType, typename ValueType>
  void UpdateShaderInput(UsdBridgeUsdWriter* writer, UsdStageRefPtr sceneStage, UsdStageRefPtr timeVarStage,
    UsdShadeShader& uniformShadPrim, UsdShadeShader& timeVarShadPrim, const SdfPath& matPrimPath, const UsdGeomPrimvarsAPI& boundGeomPrimvars, const TimeEvaluator<UsdBridgeMaterialData>& timeEval,
    UsdBridgeMaterialData::DataMemberId dataMemberId, const UsdBridgeMaterialData::MaterialInput<InputValueType>& param, const ValueType& inputValue)
  {
    const TfToken& inputToken = GetMaterialShaderInputToken<PreviewSurface>(dataMemberId);
    UpdateShaderInput<PreviewSurface>(writer, sceneStage, timeVarStage, uniformShadPrim, timeVarShadPrim, matPrimPath, boundGeomPrimvars, timeEval,
      dataMemberId, param, inputToken, inputValue);
  }

  template<bool PreviewSurface, typename InputValueType>
  void UpdateShaderInput(UsdBridgeUsdWriter* writer, UsdStageRefPtr sceneStage, UsdStageRefPtr timeVarStage,
    UsdShadeShader& uniformShadPrim, UsdShadeShader& timeVarShadPrim, const SdfPath& matPrimPath, const UsdGeomPrimvarsAPI& boundGeomPrimvars, const TimeEvaluator<UsdBridgeMaterialData>& timeEval,
    UsdBridgeMaterialData::DataMemberId dataMemberId, const UsdBridgeMaterialData::MaterialInput<InputValueType>& param)
  {
    const TfToken& inputToken = GetMaterialShaderInputToken<PreviewSurface>(dataMemberId);
    UpdateShaderInput<PreviewSurface>(writer, sceneStage, timeVarStage, uniformShadPrim, timeVarShadPrim, matPrimPath, boundGeomPrimvars, timeEval,
      dataMemberId, param, inputToken, param.Value);
  }

  template<bool PreviewSurface>
  void UpdateShaderInput_Sampler(const UsdShadeShader& uniformShader, const UsdShadeShader& timeVarShader,
    const UsdShadeShader& refSampler, const UsdSamplerRefData& samplerRefData, const ShadeGraphTypeConversionNodeContext& conversionContext)
  {
    using DMI = UsdBridgeMaterialData::DataMemberId;
    DMI samplerDMI = samplerRefData.DataMemberId;

    const TfToken& inputToken = GetMaterialShaderInputToken<PreviewSurface>(samplerDMI);
    const TfToken& outputToken = GetSamplerOutputColorToken<PreviewSurface>(samplerRefData.ImageNumComponents);
    UpdateShaderInput_ShadeNode<PreviewSurface>(uniformShader, timeVarShader, inputToken, refSampler, outputToken, conversionContext);
  }

  template<bool PreviewSurface>
  void UpdateShaderInputColorOpacity_Sampler(const UsdShadeShader& uniformShader, const UsdShadeShader& timeVarShader,
    const UsdShadeShader& refSampler, ShadeGraphTypeConversionNodeContext& conversionContext)
  {
    using DMI = UsdBridgeMaterialData::DataMemberId;

    TfToken opacityInputToken;
    TfToken opacityOutputToken;
    if(PreviewSurface)
    {
      opacityInputToken = GetMaterialShaderInputToken<PreviewSurface>(DMI::OPACITY);
      opacityOutputToken = UsdBridgeTokens->a;
    }
    else
    {
      opacityInputToken = UsdBridgeTokens->a;
      opacityOutputToken = UsdBridgeTokens->out;
    }

    UpdateShaderInput_ShadeNode<PreviewSurface>(uniformShader, timeVarShader, opacityInputToken, refSampler, opacityOutputToken, conversionContext);
  }

  template<bool PreviewSurface>
  void UpdateSamplerInputs(UsdStageRefPtr sceneStage, UsdStageRefPtr timeVarStage, const SdfPath& samplerPrimPath, const UsdBridgeSamplerData& samplerData,
    const char* imgFileName, const TfToken& attributeNameToken, const TimeEvaluator<UsdBridgeSamplerData>& timeEval)
  {
    typedef UsdBridgeSamplerData::DataMemberId DMI;

    SdfPath tcReaderPrimPath = samplerPrimPath.AppendPath(SdfPath(constring::texCoordReaderPrimPf));

    UsdShadeShader uniformTcReaderPrim = UsdShadeShader::Get(sceneStage, tcReaderPrimPath);
    assert(uniformTcReaderPrim);

    UsdShadeShader uniformSamplerPrim = UsdShadeShader::Get(sceneStage, samplerPrimPath);
    assert(uniformSamplerPrim);

    UsdShadeShader timeVarSamplerPrim = UsdShadeShader::Get(timeVarStage, samplerPrimPath);
    assert(timeVarSamplerPrim);

    SdfAssetPath texFile(imgFileName);

    if(PreviewSurface)
    {
      SetShaderInput(uniformSamplerPrim, timeVarSamplerPrim, timeEval, UsdBridgeTokens->file, DMI::DATA, texFile);
      SetShaderInput(uniformSamplerPrim, timeVarSamplerPrim, timeEval, UsdBridgeTokens->WrapS, DMI::WRAPS, TextureWrapToken(samplerData.WrapS));
      if((uint32_t)samplerData.Type >= (uint32_t)UsdBridgeSamplerData::SamplerType::SAMPLER_2D)
        SetShaderInput(uniformSamplerPrim, timeVarSamplerPrim, timeEval, UsdBridgeTokens->WrapT, DMI::WRAPT, TextureWrapToken(samplerData.WrapT));
      if((uint32_t)samplerData.Type >= (uint32_t)UsdBridgeSamplerData::SamplerType::SAMPLER_3D)
        SetShaderInput(uniformSamplerPrim, timeVarSamplerPrim, timeEval, UsdBridgeTokens->WrapR, DMI::WRAPR, TextureWrapToken(samplerData.WrapR));

      int numComponents = samplerData.ImageNumComponents;
      const TfToken& outputToken = GetSamplerOutputColorToken<true>(numComponents);
      if(!uniformSamplerPrim.GetOutput(outputToken))
      {
        uniformSamplerPrim.GetPrim().RemoveProperty(QualifiedOutputTokens->r);
        uniformSamplerPrim.GetPrim().RemoveProperty(QualifiedOutputTokens->rg);
        uniformSamplerPrim.GetPrim().RemoveProperty(QualifiedOutputTokens->rgb);

        uniformSamplerPrim.CreateOutput(outputToken, GetSamplerOutputColorType<true>(numComponents));
      }
    }
    else
    {
      SetShaderInput(uniformSamplerPrim, timeVarSamplerPrim, timeEval, UsdBridgeTokens->tex, DMI::DATA, texFile);
      SetShaderInput(uniformSamplerPrim, timeVarSamplerPrim, timeEval, UsdBridgeTokens->wrap_u, DMI::WRAPS, TextureWrapInt(samplerData.WrapS));
      if((uint32_t)samplerData.Type >= (uint32_t)UsdBridgeSamplerData::SamplerType::SAMPLER_2D)
        SetShaderInput(uniformSamplerPrim, timeVarSamplerPrim, timeEval, UsdBridgeTokens->wrap_v, DMI::WRAPT, TextureWrapInt(samplerData.WrapT));
      if((uint32_t)samplerData.Type >= (uint32_t)UsdBridgeSamplerData::SamplerType::SAMPLER_3D)
        SetShaderInput(uniformSamplerPrim, timeVarSamplerPrim, timeEval, UsdBridgeTokens->wrap_w, DMI::WRAPR, TextureWrapInt(samplerData.WrapR));

      int numComponents = samplerData.ImageNumComponents;
      const TfToken& assetIdent = GetSamplerAssetSubId(numComponents);
      const SdfValueTypeName& outputType = GetSamplerOutputColorType<false>(numComponents);
      if(uniformSamplerPrim.GetOutput(UsdBridgeTokens->out).GetTypeName() != outputType)
      {
        uniformSamplerPrim.SetSourceAssetSubIdentifier(assetIdent, UsdBridgeTokens->mdl);
        uniformSamplerPrim.GetPrim().RemoveProperty(QualifiedOutputTokens->out);
        uniformSamplerPrim.CreateOutput(UsdBridgeTokens->out, outputType);
      }
    }

    UpdateAttributeReaderName<PreviewSurface>(uniformTcReaderPrim, attributeNameToken);
  }

  void GetMaterialCoreShader(UsdStageRefPtr sceneStage, UsdStageRefPtr materialStage, const SdfPath& shaderPrimPath,
    UsdShadeShader& uniformShader, UsdShadeShader& timeVarShader)
  {
    uniformShader = UsdShadeShader::Get(sceneStage, shaderPrimPath);
    assert(uniformShader);
    timeVarShader = UsdShadeShader::Get(materialStage, shaderPrimPath);
    assert(timeVarShader);
  }

  template<bool PreviewSurface>
  void GetRefSamplerPrim(UsdStageRefPtr sceneStage, const SdfPath& refSamplerPrimPath, UsdShadeShader& refSampler)
  {
    SdfPath refSamplerPrimPath_Child = refSamplerPrimPath.AppendPath(SdfPath(PreviewSurface ? constring::psSamplerPrimPf : constring::mdlSamplerPrimPf)); // type inherited from sampler prim (in AddRef)
    refSampler = UsdShadeShader::Get(sceneStage, refSamplerPrimPath_Child);
    assert(refSampler);
  }
}

UsdShadeMaterial UsdBridgeUsdWriter::InitializeUsdMaterial(UsdStageRefPtr materialStage, const SdfPath& matPrimPath, bool uniformPrim) const
{
  return InitializeUsdMaterial_Impl(materialStage, matPrimPath, uniformPrim, Settings);
}

void UsdBridgeUsdWriter::InitializeUsdSampler(UsdStageRefPtr samplerStage,const SdfPath& samplerPrimPath, UsdBridgeSamplerData::SamplerType type, bool uniformPrim) const
{
  InitializeSampler_Impl(samplerStage, samplerPrimPath, type, uniformPrim, Settings);
}

#ifdef USE_INDEX_MATERIALS
namespace
{
  UsdShadeOutput InitializeIndexShaderUniform(UsdStageRefPtr volumeStage, UsdShadeShader& indexShader, UsdPrim& colorMapPrim)
  {
    UsdShadeConnectableAPI colorMapConn(colorMapPrim);
    UsdShadeOutput colorMapShadOut = colorMapConn.CreateOutput(UsdBridgeTokens->colormap, SdfValueTypeNames->Token);

    colorMapPrim.CreateAttribute(UsdBridgeTokens->domainBoundaryMode , SdfValueTypeNames->Token).Set(UsdBridgeTokens->clampToEdge);
    colorMapPrim.CreateAttribute(UsdBridgeTokens->colormapSource , SdfValueTypeNames->Token).Set(UsdBridgeTokens->colormapValues);

    indexShader.CreateInput(UsdBridgeTokens->colormap, SdfValueTypeNames->Token).ConnectToSource(colorMapShadOut);
    return indexShader.CreateOutput(UsdBridgeTokens->volume, SdfValueTypeNames->Token);
  }

  void InitializeIndexShaderTimeVar(UsdPrim& colorMapPrim, const TimeEvaluator<UsdBridgeVolumeData>* timeEval = nullptr)
  {
    typedef UsdBridgeVolumeData::DataMemberId DMI;

    CREATE_REMOVE_TIMEVARYING_ATTRIB(colorMapPrim, DMI::TFVALUERANGE, UsdBridgeTokens->domain, SdfValueTypeNames->Float2);

    CREATE_REMOVE_TIMEVARYING_ATTRIB(colorMapPrim, (DMI::TFCOLORS | DMI::TFOPACITIES), UsdBridgeTokens->colormapValues, SdfValueTypeNames->Float4Array);
  }
}

UsdShadeMaterial UsdBridgeUsdWriter::InitializeIndexVolumeMaterial_Impl(UsdStageRefPtr volumeStage,
  const SdfPath& volumePath, bool uniformPrim, const TimeEvaluator<UsdBridgeVolumeData>* timeEval) const
{
  SdfPath indexMatPath = volumePath.AppendPath(SdfPath(constring::indexMaterialPf));
  SdfPath indexShadPath = indexMatPath.AppendPath(SdfPath(constring::indexShaderPf));
  SdfPath colorMapPath = indexMatPath.AppendPath(SdfPath(constring::indexColorMapPf));

  UsdShadeMaterial indexMaterial = GetOrDefinePrim<UsdShadeMaterial>(volumeStage, indexMatPath);
  assert(indexMaterial);

  UsdPrim colorMapPrim = volumeStage->GetPrimAtPath(colorMapPath);
  if(!colorMapPrim)
    colorMapPrim = volumeStage->DefinePrim(colorMapPath, UsdBridgeTokens->Colormap);
  assert(colorMapPrim);

  if(uniformPrim)
  {
    UsdShadeShader indexShader = GetOrDefinePrim<UsdShadeShader>(volumeStage, indexShadPath);
    assert(indexShader);

    UsdShadeOutput indexShaderOutput = InitializeIndexShaderUniform(volumeStage, indexShader, colorMapPrim);
    indexMaterial.CreateVolumeOutput(UsdBridgeTokens->nvindex).ConnectToSource(indexShaderOutput);
  }

  InitializeIndexShaderTimeVar(colorMapPrim, timeEval);

  return indexMaterial;
}
#endif

#ifdef VALUE_CLIP_RETIMING
void UsdBridgeUsdWriter::UpdateUsdMaterialManifest(const UsdBridgePrimCache* cacheEntry, const UsdBridgeMaterialData& matData)
{
  TimeEvaluator<UsdBridgeMaterialData> timeEval(matData);
  InitializeUsdMaterial_Impl(cacheEntry->ManifestStage.second, cacheEntry->PrimPath, false,
    Settings, &timeEval);

  InitializeAttributeReaders_Impl(cacheEntry->ManifestStage.second, cacheEntry->PrimPath,
    Settings, matData, &timeEval);

  if(this->EnableSaving)
    cacheEntry->ManifestStage.second->Save();
  else
    this->TrackStageMemory(cacheEntry->ManifestStage.first + " (manifest)", cacheEntry->ManifestStage.second);
}

void UsdBridgeUsdWriter::UpdateUsdSamplerManifest(const UsdBridgePrimCache* cacheEntry, const UsdBridgeSamplerData& samplerData)
{
  TimeEvaluator<UsdBridgeSamplerData> timeEval(samplerData);

  InitializeSampler_Impl(cacheEntry->ManifestStage.second, cacheEntry->PrimPath, samplerData.Type,
    false, Settings, &timeEval);

  if(this->EnableSaving)
    cacheEntry->ManifestStage.second->Save();
  else
    this->TrackStageMemory(cacheEntry->ManifestStage.first + " (manifest)", cacheEntry->ManifestStage.second);
}
#endif

void UsdBridgeUsdWriter::ConnectSamplersToMaterial(UsdStageRefPtr materialStage, const SdfPath& matPrimPath, const SdfPrimPathList& refSamplerPrimPaths,
    const UsdBridgePrimCacheList& samplerCacheEntries, const UsdSamplerRefData* samplerRefDatas, size_t numSamplers, double worldTimeStep)
{
  typedef UsdBridgeMaterialData::DataMemberId DMI;

  if(Settings.EnablePreviewSurfaceShader)
  {
    SdfPath shaderPrimPath = matPrimPath.AppendPath(SdfPath(constring::psShaderPrimPf));

    UsdShadeShader uniformShad, timeVarShad;
    GetMaterialCoreShader(this->SceneStage, materialStage, shaderPrimPath,
      uniformShad, timeVarShad);

    for(int i = 0; i < numSamplers; ++i)
    {
      const char* samplerPrimName = samplerCacheEntries[i]->Name.GetString().c_str();

      UsdShadeShader refSampler;
      GetRefSamplerPrim<true>(this->SceneStage, refSamplerPrimPaths[i], refSampler);

      ShadeGraphTypeConversionNodeContext conversionContext(
          this->SceneStage, matPrimPath, samplerPrimName);

      UpdateShaderInput_Sampler<true>(uniformShad, timeVarShad, refSampler, samplerRefDatas[i], conversionContext);

      bool affectsOpacity = samplerRefDatas[i].DataMemberId == DMI::DIFFUSE && samplerRefDatas[i].ImageNumComponents == 4;
      if(affectsOpacity)
        UpdateShaderInputColorOpacity_Sampler<true>(uniformShad, timeVarShad, refSampler, conversionContext);
    }
  }

  if(Settings.EnableMdlShader)
  {
    SdfPath shaderPrimPath = matPrimPath.AppendPath(SdfPath(constring::mdlShaderPrimPf));
    SdfPath opMulPrimPath = matPrimPath.AppendPath(SdfPath(constring::mdlOpacityMulPrimPf));

    UsdShadeShader uniformShad, timeVarShad;
    GetMaterialCoreShader(this->SceneStage, materialStage, shaderPrimPath,
      uniformShad, timeVarShad);

    UsdShadeShader uniformOpMul = UsdShadeShader::Get(this->SceneStage, opMulPrimPath);
    UsdShadeShader timeVarOpMul = UsdShadeShader::Get(materialStage, opMulPrimPath);

    for(int i = 0; i < numSamplers; ++i)
    {
      const char* samplerPrimName = samplerCacheEntries[i]->Name.GetString().c_str();

      UsdShadeShader refSampler;
      GetRefSamplerPrim<false>(this->SceneStage, refSamplerPrimPaths[i], refSampler);

      ShadeGraphTypeConversionNodeContext conversionContext(
        this->SceneStage, matPrimPath, samplerPrimName);

      if(samplerRefDatas[i].DataMemberId != DMI::OPACITY)
      {
        UpdateShaderInput_Sampler<false>(uniformShad, timeVarShad, refSampler, samplerRefDatas[i], conversionContext);

        bool affectsOpacity = samplerRefDatas[i].DataMemberId == DMI::DIFFUSE;
        if(affectsOpacity)
        {
          if(samplerRefDatas[i].ImageNumComponents == 4)
          {
            conversionContext.ConnectionIdentifier = constring::mdlDiffuseOpacityPrimPf;
            conversionContext.ChannelSelector = 3; // Select the w channel from the float4 input

            UpdateShaderInputColorOpacity_Sampler<false>(uniformOpMul, timeVarOpMul, refSampler, conversionContext);
          }
          else
            UpdateShaderInputColorOpacity_Constant<false>(this->SceneStage, matPrimPath);
        }
      }
      else
      {
        UpdateShaderInput_ShadeNode<false>(uniformOpMul, timeVarOpMul, UsdBridgeTokens->b, refSampler, UsdBridgeTokens->out, conversionContext);
      }
    }
  }
}

void UsdBridgeUsdWriter::UpdateUsdMaterial(UsdStageRefPtr timeVarStage, const SdfPath& matPrimPath, const UsdBridgeMaterialData& matData, const UsdGeomPrimvarsAPI& boundGeomPrimvars, double timeStep)
{
  if(Settings.EnablePreviewSurfaceShader)
  {
    this->UpdatePsShader(timeVarStage, matPrimPath, matData, boundGeomPrimvars, timeStep);
  }

  if(Settings.EnableMdlShader)
  {
    this->UpdateMdlShader(timeVarStage, matPrimPath, matData, boundGeomPrimvars, timeStep);
  }
}

#define UPDATE_USD_SHADER_INPUT_MACRO(...) \
  UpdateShaderInput<true>(this, SceneStage, timeVarStage, uniformShadPrim, timeVarShadPrim, matPrimPath, boundGeomPrimvars, timeEval, __VA_ARGS__)

void UsdBridgeUsdWriter::UpdatePsShader(UsdStageRefPtr timeVarStage, const SdfPath& matPrimPath, const UsdBridgeMaterialData& matData, const UsdGeomPrimvarsAPI& boundGeomPrimvars, double timeStep)
{
  TimeEvaluator<UsdBridgeMaterialData> timeEval(matData, timeStep);
  typedef UsdBridgeMaterialData::DataMemberId DMI;

  SdfPath shadPrimPath = matPrimPath.AppendPath(SdfPath(constring::psShaderPrimPf));

  UsdShadeShader uniformShadPrim = UsdShadeShader::Get(SceneStage, shadPrimPath);
  assert(uniformShadPrim);

  UsdShadeShader timeVarShadPrim = UsdShadeShader::Get(timeVarStage, shadPrimPath);
  assert(timeVarShadPrim);

  GfVec3f difColor(GetValuePtr(matData.Diffuse));
  GfVec3f emColor(GetValuePtr(matData.Emissive));
  emColor *= matData.EmissiveIntensity.Value; // This multiplication won't translate to vcr/sampler usage


  UPDATE_USD_SHADER_INPUT_MACRO(DMI::DIFFUSE, matData.Diffuse, difColor);
  UPDATE_USD_SHADER_INPUT_MACRO(DMI::EMISSIVECOLOR, matData.Emissive, emColor);
  UPDATE_USD_SHADER_INPUT_MACRO(DMI::ROUGHNESS, matData.Roughness);
  UPDATE_USD_SHADER_INPUT_MACRO(DMI::OPACITY, matData.Opacity);
  UPDATE_USD_SHADER_INPUT_MACRO(DMI::METALLIC, matData.Metallic);
  UPDATE_USD_SHADER_INPUT_MACRO(DMI::IOR, matData.Ior);

  float opacityCutoffValue = (matData.AlphaMode == UsdBridgeMaterialData::AlphaModes::MASK) ? matData.AlphaCutoff : 0.0f; // don't enable cutoff when not explicitly specified
  SetShaderInput(uniformShadPrim, timeVarShadPrim, timeEval, UsdBridgeTokens->opacityThreshold, DMI::OPACITY, opacityCutoffValue);
}

#define UPDATE_MDL_SHADER_INPUT_MACRO(...) \
  UpdateShaderInput<false>(this, SceneStage, timeVarStage, uniformShadPrim, timeVarShadPrim, matPrimPath, boundGeomPrimvars, timeEval, __VA_ARGS__)

void UsdBridgeUsdWriter::UpdateMdlShader(UsdStageRefPtr timeVarStage, const SdfPath& matPrimPath, const UsdBridgeMaterialData& matData, const UsdGeomPrimvarsAPI& boundGeomPrimvars, double timeStep)
{
  TimeEvaluator<UsdBridgeMaterialData> timeEval(matData, timeStep);
  typedef UsdBridgeMaterialData::DataMemberId DMI;

  SdfPath shadPrimPath = matPrimPath.AppendPath(SdfPath(constring::mdlShaderPrimPf));

  UsdShadeShader uniformShadPrim = UsdShadeShader::Get(SceneStage, shadPrimPath);
  assert(uniformShadPrim);
  UsdShadeShader timeVarShadPrim = UsdShadeShader::Get(timeVarStage, shadPrimPath);
  assert(timeVarShadPrim);

  SdfPath opMulPrimPath = matPrimPath.AppendPath(SdfPath(constring::mdlOpacityMulPrimPf));

  UsdShadeShader uniformOpMulPrim = UsdShadeShader::Get(SceneStage, opMulPrimPath);
  assert(uniformOpMulPrim);
  UsdShadeShader timeVarOpMulPrim = UsdShadeShader::Get(timeVarStage, opMulPrimPath);
  assert(timeVarOpMulPrim);

  GfVec3f difColor(GetValuePtr(matData.Diffuse));
  GfVec3f emColor(GetValuePtr(matData.Emissive));

  bool enableEmission = (matData.EmissiveIntensity.Value >= 0.0 || matData.EmissiveIntensity.SrcAttrib || matData.EmissiveIntensity.Sampler);

  UPDATE_MDL_SHADER_INPUT_MACRO(DMI::DIFFUSE, matData.Diffuse, difColor);
  UPDATE_MDL_SHADER_INPUT_MACRO(DMI::EMISSIVECOLOR, matData.Emissive, emColor);
  UPDATE_MDL_SHADER_INPUT_MACRO(DMI::EMISSIVEINTENSITY, matData.EmissiveIntensity);
  UPDATE_MDL_SHADER_INPUT_MACRO(DMI::ROUGHNESS, matData.Roughness);
  UPDATE_MDL_SHADER_INPUT_MACRO(DMI::METALLIC, matData.Metallic);
  UpdateShaderInput<false>(this, SceneStage, timeVarStage, uniformOpMulPrim, timeVarOpMulPrim, matPrimPath, boundGeomPrimvars,
    timeEval, DMI::OPACITY, matData.Opacity, UsdBridgeTokens->b, matData.Opacity.Value);

  float opacityCutoffValue = (matData.AlphaMode == UsdBridgeMaterialData::AlphaModes::MASK) ? matData.AlphaCutoff : 0.0f; // don't enable cutoff when not explicitly specified
  bool enableOpacity = matData.AlphaMode != UsdBridgeMaterialData::AlphaModes::NONE;
  SetShaderInput(uniformShadPrim, timeVarShadPrim, timeEval, UsdBridgeTokens->opacity_threshold, DMI::OPACITY, opacityCutoffValue);
  SetShaderInput(uniformShadPrim, timeVarShadPrim, timeEval, UsdBridgeTokens->enable_opacity, DMI::OPACITY, enableOpacity);
  SetShaderInput(uniformShadPrim, timeVarShadPrim, timeEval, UsdBridgeTokens->enable_emission, DMI::EMISSIVEINTENSITY, enableEmission);

#ifdef CUSTOM_PBR_MDL
  if (!enableOpacity)
    uniformShadPrim.SetSourceAsset(this->MdlOpaqueRelFilePath, UsdBridgeTokens->mdl);
  else
    uniformShadPrim.SetSourceAsset(this->MdlTranslucentRelFilePath, UsdBridgeTokens->mdl);
#endif
}

void UsdBridgeUsdWriter::UpdateUsdSampler(UsdStageRefPtr timeVarStage, UsdBridgePrimCache* cacheEntry, const UsdBridgeSamplerData& samplerData, double timeStep)
{
  TimeEvaluator<UsdBridgeSamplerData> timeEval(samplerData, timeStep);
  typedef UsdBridgeSamplerData::DataMemberId DMI;

  const SdfPath& samplerPrimPath = cacheEntry->PrimPath;

  const std::string& defaultName = cacheEntry->Name.GetString();
  const std::string& generatedFileName = GetResourceFileName(constring::imgFolder, samplerData.ImageName, defaultName, timeStep, constring::imageExtension);

  const char* imgFileName = samplerData.ImageUrl;
  bool writeFile = false; // No resource key is stored if no file is written
  if(!imgFileName)
  {
    imgFileName = generatedFileName.c_str();
    writeFile = true;
  }

  const TfToken& attribNameToken = AttributeNameToken(samplerData.InAttribute);

  if(Settings.EnablePreviewSurfaceShader)
  {
    SdfPath usdSamplerPrimPath = samplerPrimPath.AppendPath(SdfPath(constring::psSamplerPrimPf));
    UpdateSamplerInputs<true>(SceneStage, timeVarStage, usdSamplerPrimPath, samplerData, imgFileName, attribNameToken, timeEval);
  }

  if(Settings.EnableMdlShader)
  {
    SdfPath usdSamplerPrimPath = samplerPrimPath.AppendPath(SdfPath(constring::mdlSamplerPrimPf));
    UpdateSamplerInputs<false>(SceneStage, timeVarStage, usdSamplerPrimPath, samplerData, imgFileName, attribNameToken, timeEval);
  }

  if(writeFile)
  {
    const char* resourceName = samplerData.ImageName ? samplerData.ImageName : defaultName.c_str();
    UsdBridgeResourceKey key(resourceName, timeStep);
    bool newEntry = cacheEntry->AddResourceKey(key);

    bool isSharedResource = samplerData.ImageName;
    if(newEntry && isSharedResource)
      AddSharedResourceRef(key);

    assert(samplerData.Data);
    if(!isSharedResource || !SetSharedResourceModified(key))
    {
      TempImageData.resize(0);

      const void* convertedSamplerData = nullptr;
      int64_t convertedSamplerStride = samplerData.ImageStride[1];
      int numComponents = samplerData.ImageNumComponents;

      UsdBridgeType flattenedType = ubutils::UsdBridgeTypeFlatten(samplerData.DataType);
      if( !(flattenedType == UsdBridgeType::UCHAR || flattenedType == UsdBridgeType::UCHAR_SRGB_R))
      {
        ConvertSamplerDataToImage(samplerData, TempImageData);

        if(TempImageData.size())
        {
          convertedSamplerData = TempImageData.data();
          convertedSamplerStride = samplerData.ImageDims[0]*numComponents;
        }
      }
      else
      {
        convertedSamplerData = samplerData.Data;
      }

      if(numComponents <= 4 && convertedSamplerData)
      {
        StbWriteOutput writeOutput;

        stbi_write_png_to_func(StbWriteToBuffer, &writeOutput,
          static_cast<int>(samplerData.ImageDims[0]), static_cast<int>(samplerData.ImageDims[1]),
          numComponents, convertedSamplerData, convertedSamplerStride);

        std::string wdRelFilename(SessionDirectory + imgFileName);
        Connect->WriteFile(writeOutput.imageData, writeOutput.imageSize, wdRelFilename.c_str(), true);

        if(!this->EnableSaving && g_rankMemoryStore)
        {
          GetDiffCapture().CapturePreStore(imgFileName);
          g_rankMemoryStore->StoreFile(
            imgFileName,
            writeOutput.imageData,
            writeOutput.imageSize,
            "image/png"
          );
        }
      }
      else
      {
        UsdBridgeLogMacro(this->LogObject, UsdBridgeLogLevel::WARNING, "Image file not written out, format not supported (should be 1-4 component unsigned char/short/int or float/double): " << samplerData.ImageName);
      }
    }
  }
}

namespace
{
  template<bool PreviewSurface>
  void UpdateAttributeReader_Impl(UsdStageRefPtr sceneStage, UsdStageRefPtr timeVarStage, const SdfPath& matPrimPath,
    UsdBridgeMaterialData::DataMemberId dataMemberId, const TfToken& newNameToken, const UsdGeomPrimvarsAPI& boundGeomPrimvars,
    const TimeEvaluator<UsdBridgeMaterialData>& timeEval, UsdBridgeSettings& settings, const UsdBridgeLogObject& logObj)
  {
    typedef UsdBridgeMaterialData::DataMemberId DMI;

    if(PreviewSurface && dataMemberId == DMI::EMISSIVEINTENSITY)
      return;

    SdfPath attributeReaderPath = matPrimPath.AppendPath(GetAttributeReaderPathPf<PreviewSurface>(dataMemberId));

    UsdShadeShader uniformAttribReader = UsdShadeShader::Get(sceneStage, attributeReaderPath);
    if(!uniformAttribReader)
    {
      UsdBridgeLogMacro(logObj, UsdBridgeLogLevel::ERR, "In UpdateAttributeReader_Impl(): requesting an attribute reader that hasn't been created during fixup of name token.");
      return;
    }

    UpdateAttributeReaderName<PreviewSurface>(uniformAttribReader, newNameToken);
    UpdateAttributeReaderOutput<PreviewSurface>(uniformAttribReader, boundGeomPrimvars, newNameToken);

    UsdShadeShader uniformTargetPrim;
    TfToken inputToken;

    if(!PreviewSurface && dataMemberId == DMI::OPACITY)
    {
      SdfPath opMulPrimPath = matPrimPath.AppendPath(SdfPath(constring::mdlOpacityMulPrimPf));
      uniformTargetPrim = UsdShadeShader::Get(sceneStage, opMulPrimPath);
      inputToken = UsdBridgeTokens->b;
    }
    else
    {
      SdfPath shadPrimPath = matPrimPath.AppendPath(SdfPath(
        PreviewSurface ? constring::psShaderPrimPf : constring::mdlShaderPrimPf));
      uniformTargetPrim = UsdShadeShader::Get(sceneStage, shadPrimPath);
      inputToken = GetMaterialShaderInputToken<PreviewSurface>(dataMemberId);
    }

    ShadeGraphTypeConversionNodeContext conversionContext(
      sceneStage, matPrimPath, GetAttributeReaderPathPf<PreviewSurface>(dataMemberId).GetString().c_str());

    const TfToken& outputToken = PreviewSurface ? UsdBridgeTokens->result : UsdBridgeTokens->out;
    UpdateShaderInput_ShadeNode<PreviewSurface>(uniformTargetPrim, UsdShadeShader(), inputToken,
      uniformAttribReader, outputToken, conversionContext);

    UpdateShaderIndirectInput<PreviewSurface>(sceneStage, timeVarStage,
      matPrimPath, boundGeomPrimvars, newNameToken, uniformAttribReader, dataMemberId, settings);
  }

  template<bool PreviewSurface>
  void UpdateSamplerTcReader(UsdStageRefPtr sceneStage, UsdStageRefPtr timeVarStage, const SdfPath& samplerPrimPath, const TfToken& newNameToken, const TimeEvaluator<UsdBridgeSamplerData>& timeEval)
  {
    typedef UsdBridgeSamplerData::DataMemberId DMI;

    SdfPath tcReaderPrimPath = samplerPrimPath.AppendPath(SdfPath(constring::texCoordReaderPrimPf));

    UsdShadeShader uniformTcReaderPrim = UsdShadeShader::Get(sceneStage, tcReaderPrimPath);
    assert(uniformTcReaderPrim);

    UpdateAttributeReaderName<PreviewSurface>(uniformTcReaderPrim, newNameToken);
  }
}

void UsdBridgeUsdWriter::UpdateAttributeReader(UsdStageRefPtr timeVarStage, const SdfPath& matPrimPath, MaterialDMI dataMemberId, const char* newName, const UsdGeomPrimvarsAPI& boundGeomPrimvars, double timeStep, MaterialDMI timeVarying)
{
  UsdBridgeMaterialData materialData;
  materialData.TimeVarying = timeVarying;

  TimeEvaluator<UsdBridgeMaterialData> timeEval(materialData, timeStep);

  const TfToken& newNameToken = AttributeNameToken(newName);

  if(Settings.EnablePreviewSurfaceShader)
  {
    UpdateAttributeReader_Impl<true>(SceneStage, timeVarStage, matPrimPath, dataMemberId, newNameToken, boundGeomPrimvars, timeEval, Settings, this->LogObject);
  }

  if(Settings.EnableMdlShader)
  {
    UpdateAttributeReader_Impl<false>(SceneStage, timeVarStage, matPrimPath, dataMemberId, newNameToken, boundGeomPrimvars, timeEval, Settings, this->LogObject);
  }
}

void UsdBridgeUsdWriter::UpdateInAttribute(UsdStageRefPtr timeVarStage, const SdfPath& samplerPrimPath, const char* newName, double timeStep, SamplerDMI timeVarying)
{
  UsdBridgeSamplerData samplerData;
  samplerData.TimeVarying = timeVarying;

  TimeEvaluator<UsdBridgeSamplerData> timeEval(samplerData, timeStep);

  const TfToken& newNameToken = AttributeNameToken(newName);

  if(Settings.EnablePreviewSurfaceShader)
  {
    SdfPath usdSamplerPrimPath = samplerPrimPath.AppendPath(SdfPath(constring::psSamplerPrimPf));
    UpdateSamplerTcReader<true>(SceneStage, timeVarStage, usdSamplerPrimPath, newNameToken, timeEval);
  }

  if(Settings.EnableMdlShader)
  {
    SdfPath usdSamplerPrimPath = samplerPrimPath.AppendPath(SdfPath(constring::mdlSamplerPrimPf));
    UpdateSamplerTcReader<false>(SceneStage, timeVarStage, usdSamplerPrimPath, newNameToken, timeEval);
  }
}

#ifdef USE_INDEX_MATERIALS
void UsdBridgeUsdWriter::UpdateIndexVolumeMaterial(UsdStageRefPtr sceneStage, UsdStageRefPtr timeVarStage, const SdfPath& volumePath, const UsdBridgeVolumeData& volumeData, double timeStep)
{
  TimeEvaluator<UsdBridgeVolumeData> timeEval(volumeData, timeStep);
  typedef UsdBridgeVolumeData::DataMemberId DMI;

  SdfPath indexMatPath = volumePath.AppendPath(SdfPath(constring::indexMaterialPf));
  SdfPath colorMapPath = indexMatPath.AppendPath(SdfPath(constring::indexColorMapPf));

  GfVec2f valueRange(GfVec2d(volumeData.TfData.TfValueRange));

  assert(volumeData.TfData.TfOpacitiesType == UsdBridgeType::FLOAT);
  const float* tfOpacities = static_cast<const float*>(volumeData.TfData.TfOpacities);

  UsdPrim uniformColorMap = sceneStage->GetPrimAtPath(colorMapPath);
  assert(uniformColorMap);
  UsdPrim timeVarColorMap = timeVarStage->GetPrimAtPath(colorMapPath);
  assert(timeVarColorMap);

  UsdAttribute uniformDomainAttr = uniformColorMap.GetAttribute(UsdBridgeTokens->domain);
  UsdAttribute timeVarDomainAttr = timeVarColorMap.GetAttribute(UsdBridgeTokens->domain);
  UsdAttribute uniformTfValueAttr = uniformColorMap.GetAttribute(UsdBridgeTokens->colormapValues);
  UsdAttribute timeVarTfValueAttr = timeVarColorMap.GetAttribute(UsdBridgeTokens->colormapValues);

  bool rangeTimeVarying = false;//timeEval.IsTimeVarying(DMI::TFVALUERANGE);
  bool valuesTimeVarying = false;

  ClearUsdAttributes(uniformDomainAttr, timeVarDomainAttr, rangeTimeVarying);
  ClearUsdAttributes(uniformTfValueAttr, timeVarTfValueAttr, valuesTimeVarying);

  UsdAttribute& outAttrib = valuesTimeVarying ? timeVarTfValueAttr : uniformTfValueAttr;
  UsdTimeCode outTimeCode = valuesTimeVarying ? timeEval.TimeCode : timeEval.Default();

  WriteTfPrimvars(tfOpacities, volumeData, outAttrib, outTimeCode);

  SET_TIMEVARYING_ATTRIB(rangeTimeVarying, timeVarDomainAttr, uniformDomainAttr, valueRange);
}

void UsdBridgeUsdWriter::WriteTfPrimvars(const float* tfOpacities, const UsdBridgeVolumeData& volumeData, UsdAttribute& outAttrib, UsdTimeCode outTimeCode)
{
  size_t arrayNumElements = volumeData.TfData.TfNumColors;
  const void* arrayData = volumeData.TfData.TfColors;
  UsdBridgeType arrayDataType = volumeData.TfData.TfColorsType;
  UsdBridgeArrays::AttribSpanInit spanInit(arrayNumElements, outAttrib, outTimeCode);
  UsdBridgeSpanI<GfVec4f>* tfColorsSpan = UsdBridgeArrays::AssignArrayToAttribute<UsdBridgeArrays::AttribSpanInit, UsdBridgeArrays::AttribSpan, GfVec4f>(
    LogObject, nullptr, arrayDataType, arrayNumElements, spanInit);
  if(tfColorsSpan)
  {
    UsdBridgeArrays::WriteToSpanColor(this->LogObject, *tfColorsSpan, arrayData, arrayNumElements, arrayDataType);
    GfVec4f* outTfData = tfColorsSpan->begin();
    for(size_t i = 0; i < tfColorsSpan->size() && i < volumeData.TfData.TfNumOpacities; ++i, ++outTfData)
      (*outTfData)[3] = tfOpacities[i];
    tfColorsSpan->AssignToAttrib();
  }
}
#endif

void ResourceCollectSampler(UsdBridgePrimCache* cache, UsdBridgeUsdWriter& usdWriter)
{
  RemoveResourceFiles(cache, usdWriter, constring::imgFolder, constring::imageExtension);
}

