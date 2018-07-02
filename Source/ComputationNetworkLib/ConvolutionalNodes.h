//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//
#pragma once

#include "Basics.h"
#include "Globals.h"
#include "Matrix.h"
#include "ComputationNode.h"
#include "ConvolutionEngine.h"

namespace Microsoft { namespace MSR { namespace CNTK {

// -----------------------------------------------------------------------
// ConvolutionNodeBase
// -----------------------------------------------------------------------

// ConvolutionNodeBase is a base class for ND-convolution(ConvolutionNode) and ND-pooling(PoolingNode).
//
// 2D convolutions (incl. pooling) support two different storage formats:
//
// * legacy ("HWC") mode: Channels are tuples of scalars
//
//    This follows "high performance convolutional neural networks for document processing" by Kumar Chellapilla, Sidde Puri, and Patrice Simard.
//    Each sample is stored as a column-major matrix (height, width) of float[numChannels] (r00, g00, b00, r10, g10, b10, r01, g01, b01, r11, g11, b11).
//
//     - input :  [C x W  x H      x T]  or  ARRAY[1..T] OF                ARRAY[1..H]  OF ARRAY[1..W]  OF ARRAY[1..C]
//     - output : [K x W' x H'     x T]  or  ARRAY[1..T] OF                ARRAY[1..H'] OF ARRAY[1..W'] OF ARRAY[1..K]
//     - filter : [K x W" x H" x C    ]  or                 ARRAY[1..C] OF ARRAY[1..H"] OF ARRAY[1..W"] OF ARRAY[1..K]
//
// * cudnn ("CHW") mode (works both GPU and CPU): Channels are planes
//
//     - input :   [W  x H  x C      x T]   or  ARRAY[1..T] OF                ARRAY[1..C]  OF ARRAY[1..H]  OF ARRAY[1..W]
//     - output :  [W' x H' x      K x T]   or  ARRAY[1..T] OF ARRAY[1..K] OF                 ARRAY[1..H'] OF ARRAY[1..W']
//     - filter :  [W" x H" x C  x K    ]   or                 ARRAY[1..K] OF ARRAY[1..C]  OF ARRAY[1..H]  OF ARRAY[1..W]
//
// where:
//  - using ' for output and " for filter
//  - T = samples (NVidia calls this N)
//  - W, H = width, height (W', H' for output, W", H" for kernel)
//  - C = input channels
//     - 3 for color images, 1 for B&W images
//     - for hidden layer: dimension of activation vector for each pixel
//  - K = output channels = dimension of activation vector for each pixel (also called N by NVidia, inconsistently)
//
// For ND-convolution/pooling only second format ('cudnn') is supported.
//
template <class ElemType>
class ConvolutionNodeBase : public ComputationNode<ElemType>
{
    typedef ComputationNode<ElemType> Base; UsingComputationNodeMembers;

public:
    ConvolutionNodeBase(DEVICEID_TYPE deviceId, const wstring& name)
        : Base(deviceId, name), m_poolKind(PoolKind::None), m_poolIncludePad(false), m_transpose(false), m_outputShape(TensorShape(0)), m_ceilOutDim(false), m_maxTempMemSizeInSamples(0)
    {
    }
    ConvolutionNodeBase(DEVICEID_TYPE deviceId, const wstring& name, const TensorShape& kernelShape, const TensorShape& mapCount, const TensorShape& strideShape,
                        const std::vector<bool>& sharing, const std::vector<bool>& autoPadding, const TensorShape& lowerPad, const TensorShape& upperPad,
                        PoolKind poolKind, bool poolIncludePad, bool transpose, const TensorShape& outputShape, bool ceilOutDim, ImageLayoutKind imageLayout, size_t maxTempMemSizeInSamples)
                        : Base(deviceId, name), m_kernelShape(kernelShape), m_mapCount(mapCount), m_stride(strideShape), m_sharing(sharing),
                        m_autoPad(autoPadding), m_lowerPad(lowerPad), m_upperPad(upperPad), m_poolKind(poolKind), m_poolIncludePad(poolIncludePad), m_transpose(transpose), m_outputShape(outputShape),
                        m_ceilOutDim(ceilOutDim), m_imageLayout(imageLayout), m_maxTempMemSizeInSamples(maxTempMemSizeInSamples)
    {
    }

public:
    void Save(File& fstream) const override
    {
        Base::Save(fstream);

        m_kernelShape.Save(fstream);
        m_mapCount.Save(fstream);
        m_stride.Save(fstream);
        fstream << m_sharing;
        fstream << m_autoPad;
        m_lowerPad.Save(fstream);
        m_upperPad.Save(fstream);
        fstream << (int32_t)m_poolKind;
        fstream << (int32_t)m_imageLayout;
        fstream << m_maxTempMemSizeInSamples;
        fstream << m_transpose;
        m_outputShape.Save(fstream);
        fstream << m_ceilOutDim;
        fstream << m_poolIncludePad;
    }

    void Load(File& fstream, size_t modelVersion) override
    {
        Base::Load(fstream, modelVersion);

        // Let ConvolutionNode handle older models.
        if (modelVersion >= CNTK_MODEL_VERSION_5)
        {
            m_kernelShape.Load(fstream);
            m_mapCount.Load(fstream);
            m_stride.Load(fstream);
            fstream >> m_sharing;
            fstream >> m_autoPad;
            m_lowerPad.Load(fstream);
            m_upperPad.Load(fstream);
            int32_t k;
            fstream >> k;
            m_poolKind = (PoolKind)k;
            int32_t layout;
            fstream >> layout;
            m_imageLayout = (ImageLayoutKind)layout;
            fstream >> m_maxTempMemSizeInSamples;
        }
        if (modelVersion >= CNTK_MODEL_VERSION_9)
        {
            fstream >> m_transpose;
        }
        if (modelVersion >= CNTK_MODEL_VERSION_20)
        {
            m_outputShape.Load(fstream);
        }
        if (modelVersion >= CNTK_MODEL_VERSION_21)
        {
            fstream >> m_ceilOutDim;
        }
        if (modelVersion >= CNTK_MODEL_VERSION_23)
        {
            fstream >> m_poolIncludePad;
        }
    }

    void CopyTo(ComputationNodeBasePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const override
    {
        Base::CopyTo(nodeP, newName, flags);
        if (flags & CopyNodeFlags::copyNodeValue)
        {
            auto node = dynamic_pointer_cast<ConvolutionNodeBase<ElemType>>(nodeP);
            node->m_kernelShape = m_kernelShape;
            node->m_mapCount = m_mapCount;
            node->m_stride = m_stride;
            node->m_sharing = m_sharing;
            node->m_autoPad = m_autoPad;
            node->m_lowerPad = m_lowerPad;
            node->m_upperPad = m_upperPad;
            node->m_poolKind = m_poolKind;
            node->m_transpose = m_transpose;
            node->m_outputShape = m_outputShape;
            node->m_ceilOutDim = m_ceilOutDim;
            node->m_poolIncludePad = m_poolIncludePad;
            node->m_imageLayout = m_imageLayout;
            node->m_maxTempMemSizeInSamples = m_maxTempMemSizeInSamples;
        }
    }

    void DumpNodeInfo(const bool printValues, const bool printMetadata, File& fstream) const override
    {
        Base::DumpNodeInfo(printValues, printMetadata, fstream);

        if (m_convEng != nullptr)
            fstream << "Geometry: " << string(*m_convEng->Geometry()) << "\n";
        fstream << "PoolKind: " << (int)m_poolKind << "\n";
    }

    TensorShape KernelShape() const { return m_kernelShape; }
    TensorShape MapCount() const { return m_mapCount; }
    TensorShape Strides() const { return m_stride; }
    std::vector<bool> Sharing() const { return m_sharing; }
    std::vector<bool> AutoPad() const { return m_autoPad; }
    TensorShape LowerPad() const { return m_lowerPad; }
    TensorShape UpperPad() const { return m_upperPad; }
    bool Transpose() const { return m_transpose; }
    TensorShape OutputShape() const { return m_outputShape; }
    size_t MaxTempMemSizeInSamples() const { return m_maxTempMemSizeInSamples; }
    PoolKind PoolingKind() const { return m_poolKind; }
    bool CeilOutDim() const { return m_ceilOutDim; }
    bool PoolIncludePad() const { return m_poolIncludePad; }

    // bottomlessly expand shape to filterRank, then expand to inputRank using defaults or given 'from' values
    template<class V, typename T>
    static void FixVectorShape(size_t filterRank, size_t inputRank, V& shape, T deflt, const V& from = V())
    {
        if (shape.size() == 0)
            return; // let ComputeOutputShape() deal with this special case
        // repeat the last value until we have the same rank as the filter
        while (shape.size() < filterRank)
            shape.push_back(shape.back());
        // increase to input rank
        // If 'from' is given then clone the value from there. This is meant to be the input dimensions for convolution.
        while (shape.size() < inputRank)
            shape.push_back(shape.size() < from.size() ? from[shape.size()] : deflt);
    }

private:
    static void FixTensorShape(size_t filterRank, size_t inputRank, TensorShape& shape, size_t deflt, const TensorShape& from = TensorShape())
    {
        auto dims = shape.GetDims();
        FixVectorShape(filterRank, inputRank, dims, deflt, from.GetDims());
        shape = TensorShape(dims);
    }
protected:
    // infer reduction dimensions if m_convolution2D is true, for legacy NDL branch
    void InferConvolution2DReductionDims(const TensorShape& inputShape, size_t numChannels)
    {
        size_t kW = m_kernelShape[0];
        size_t kH = m_kernelShape[1];
        size_t sW = m_stride[0];
        size_t sH = m_stride[1];
        m_kernelShape = TensorShape(kW, kH, numChannels);
        m_stride = TensorShape(sW, sH, numChannels);
        size_t filterRank = 2;
        FixVectorShape(filterRank, inputShape.size(), m_autoPad, false);
        FixTensorShape(filterRank, inputShape.size(), m_lowerPad, 0);
        FixTensorShape(filterRank, inputShape.size(), m_upperPad, 0);
        FixVectorShape(filterRank, inputShape.size(), m_sharing, true);
    }

    // infer reduction dimensions if not given
    void InferReductionDims(const TensorShape& inputShape, const TensorShape& fromShape)
    {
        // If kernel has a lower rank than the input then the remaining dimensions are to be reduced over.
        size_t filterRank = m_kernelShape.size();
        FixTensorShape(filterRank, inputShape.size(), m_kernelShape, 1,     fromShape); // convolve over red dim; pool over 1
        FixTensorShape(filterRank, inputShape.size(), m_stride,      1,     fromShape); // stride for reduction dims is red dim or 1
        FixVectorShape(filterRank, inputShape.size(), m_autoPad,     false);            // no padding for reduction dims
        FixTensorShape(filterRank, inputShape.size(), m_lowerPad,    0);
        FixTensorShape(filterRank, inputShape.size(), m_upperPad,    0);
        FixVectorShape(filterRank, inputShape.size(), m_sharing,     true);
    }

    // Derived classes implement transforms calculation. Since all derived classes are filter based we consolidate common
    // filter transform calculation here to be reused by derived classes. For example convolution and de-convolution
    // have same transform but inversed, hence both of them may reuse this method and one will call inverse in addition
    // (similar holds for pooling nodes).
    SpaceTransform ComputeFilterTransform()
    {
        std::shared_ptr<const ConvolveGeometry> geometry = m_convEng->Geometry();

        SpaceTransform result;
        result.m_axisTransforms.resize(2);

        result.m_axisTransforms[0].scale = (float)(geometry->GetStride(0));
        result.m_axisTransforms[0].translate = (float)((geometry->KernelShape()[0] - 1) / 2 - geometry->GetLowerPad(0));

        result.m_axisTransforms[1].scale = (float)(geometry->GetStride(1));
        result.m_axisTransforms[1].translate = (float)((geometry->KernelShape()[1] - 1) / 2 - geometry->GetLowerPad(1));

        return result;
    }

    virtual TensorShape ComputeOutputShape(const TensorShape& inputShape, const TensorShape& dilate, bool ceilOutDim, bool isFinalValidationPass)
    {
        const size_t DEAFULT_NUM_GROUPS = 1;
        return ConvolveGeometry::ComputeOutputShape(inputShape, m_kernelShape, m_mapCount, m_stride,
            m_sharing, m_autoPad, m_lowerPad, m_upperPad, dilate, DEAFULT_NUM_GROUPS, ceilOutDim,
            Base::NeedsDynamicValidation(), isFinalValidationPass);
    }

protected:
    TensorShape m_kernelShape;
    TensorShape m_mapCount;
    TensorShape m_stride;
    std::vector<bool> m_sharing;
    std::vector<bool> m_autoPad;
    TensorShape m_lowerPad;
    TensorShape m_upperPad;
    PoolKind m_poolKind;
    bool m_transpose;
    TensorShape m_outputShape;
    bool m_ceilOutDim;
    bool m_poolIncludePad;
    ImageLayoutKind m_imageLayout;

    size_t m_maxTempMemSizeInSamples;
    shared_ptr<Matrix<ElemType>> m_tempMatrixForward;
    shared_ptr<Matrix<ElemType>> m_tempMatrixBackward;

    std::unique_ptr<ConvolutionEngine<ElemType>> m_convEng;
};

#define UsingConvolutionNodeBaseMembers     \
    UsingComputationNodeMembersBoilerplate; \
protected:                                  \
    using Base::m_kernelShape;              \
    using Base::m_mapCount;                 \
    using Base::m_stride;                   \
    using Base::m_sharing;                  \
    using Base::m_autoPad;                  \
    using Base::m_lowerPad;                 \
    using Base::m_upperPad;                 \
    using Base::m_poolKind;                 \
    using Base::m_transpose;                \
    using Base::m_outputShape;              \
    using Base::m_ceilOutDim;               \
    using Base::m_poolIncludePad;           \
    using Base::m_imageLayout;              \
    using Base::m_maxTempMemSizeInSamples;  \
    using Base::m_tempMatrixForward;        \
    using Base::m_tempMatrixBackward;       \
    using Base::m_convEng;                  \
    using Base::InferConvolution2DReductionDims; \
    using Base::InferReductionDims;         \
public:

// -----------------------------------------------------------------------
// ConvolutionNode (convolutionWeights, inputFeature)
// -----------------------------------------------------------------------

// forward declaration.
template <class ElemType>
class ConvolutionOverSequenceAxisNode;
template <class ElemType>
class ConvolutionOverSequenceAxisNodeV2;

template <class ElemType>
class ConvolutionNode : public ConvolutionNodeBase<ElemType>, public NumInputs<2>, public TransformerNode
{
    typedef ConvolutionNodeBase<ElemType> Base; UsingConvolutionNodeBaseMembers;
    static const std::wstring TypeName() { return L"Convolution"; }

    // Needs access to m_dilations and m_groups which are private members.
    template <class ElementType>
    friend class ConvolutionOverSequenceAxisNode;
    template <class ElementType>
    friend class ConvolutionOverSequenceAxisNodeV2;
public:
    ConvolutionNode(DEVICEID_TYPE deviceId, const wstring& name)
        : Base(deviceId, name), m_dilation(TensorShape(1)), m_groups(1)
    {
    }
    ConvolutionNode(DEVICEID_TYPE deviceId, const wstring& name, const TensorShape& kernelShape, const TensorShape& mapCount, const TensorShape& strideShape,
                    const std::vector<bool>& sharing, const std::vector<bool>& autoPadding, const TensorShape& lowerPad, const TensorShape& upperPad,
                    bool transpose, const TensorShape &outputShape, ImageLayoutKind imageLayout, size_t maxTempMemSizeInSamples, const TensorShape& dilation=TensorShape(1),
                    size_t groups=1)
        : Base(deviceId, name, kernelShape, mapCount, strideShape, sharing, autoPadding, lowerPad, upperPad, PoolKind::None, false, transpose, outputShape, false, imageLayout, maxTempMemSizeInSamples),
        m_convolution2D(false), m_dilation(dilation), m_groups(groups)
    {
        // Make sure not using dilation on CPU
        if(deviceId < 0)
        {
            for(int i = 0; i < dilation.size(); i++)
            {
                if(1 != dilation[i])
                    RuntimeError("Dilated convolution on CPU is not yet implemented.");
            }
        }
    }
    ConvolutionNode(DEVICEID_TYPE deviceId, const wstring& name, const size_t kernelWidth, const size_t kernelHeight, const size_t outputChannels,
                    const size_t horizontalSubsample, const size_t verticalSubsample, ImageLayoutKind imageLayout,
                    bool zeroPadding, size_t maxTempMemSizeInSamples)
                    : ConvolutionNode(deviceId, name, TensorShape(kernelWidth, kernelHeight, 1), TensorShape(1, 1, outputChannels),
                                      TensorShape(horizontalSubsample, verticalSubsample, 1), vector<bool>{true},
                                      vector<bool>{zeroPadding}, TensorShape(0), TensorShape(0),
                                      false, TensorShape(0), imageLayout, maxTempMemSizeInSamples)
    {
        m_convolution2D = true;
    }
    ConvolutionNode(const ScriptableObjects::IConfigRecordPtr configp)
        : ConvolutionNode(configp->Get(L"deviceId"), L"<placeholder>", configp->Get(L"kernelShape"), configp->Get(L"mapCount"), configp->Get(L"strideShape"),
                          configp->Get(L"dimSharing"), configp->Get(L"dimPadding"), configp->Get(L"dimPadLower"), configp->Get(L"dimPadUpper"),
                          configp->Get(L"transpose"), configp->Get(L"dimOutputShape"), ImageLayoutKindFrom(configp->Get(L"imageLayout")), configp->Get(L"maxTempMemSizeInSamples"), configp->Get(L"dimDilation"))
    {
        AttachInputsFromConfig(configp, GetExpectedNumInputs());
    }

    // TODO: the check for NeedsDynamicValidation() is a temporary resolution and needs to be properly handled when we look at support for free dimension convolution inputs.
    virtual ParentGradientOptimization ImplementsGradientOptimization(const ComputationNodeBase*) const override
    {
        bool overwrite = Base::NeedsDynamicValidation() ? false : m_convEng->ImplementsGradientOverwriteOptimization();
        return overwrite ? ParentGradientOptimization::Overwrite : ParentGradientOptimization::None;
    }

public:
    void Save(File& fstream) const override
    {
        Base::Save(fstream);
        fstream << m_convolution2D;
        m_dilation.Save(fstream);
    }

    void Load(File& fstream, size_t modelVersion) override
    {
        Base::Load(fstream, modelVersion);

        // Back compat: load pre-ND convolution models.
        if (modelVersion < CNTK_MODEL_VERSION_5)
        {
            size_t kW, kH, sW, sH;
            fstream >> kW;
            fstream >> kH;
            fstream >> sW;
            fstream >> sH;
            uint32_t imageLayout, mapCount;
            fstream >> mapCount;
            fstream >> imageLayout;
            m_imageLayout = (ImageLayoutKind)imageLayout;
            bool pad;
            fstream >> pad;
            fstream >> m_maxTempMemSizeInSamples;
            m_poolKind = PoolKind::None;
            m_convolution2D = true;

            m_kernelShape = TensorShape(kW, kH, 1);
            m_mapCount = TensorShape(mapCount);
            m_stride = TensorShape(sW, sH, 1);
            m_sharing = vector<bool>{true};
            m_autoPad = vector<bool>{pad};
            m_lowerPad = TensorShape(0);
            m_upperPad = TensorShape(0);
        }
        else
        {
            fstream >> m_convolution2D;
            if (modelVersion >= CNTK_MODEL_VERSION_18)
            {
                m_dilation.Load(fstream);
            }
            else
            {
                m_dilation = TensorShape(1);
            }
        }
    }

    void CopyTo(ComputationNodeBasePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const override
    {
        Base::CopyTo(nodeP, newName, flags);
        if (flags & CopyNodeFlags::copyNodeValue)
        {
            auto node = dynamic_pointer_cast<ConvolutionNode<ElemType>>(nodeP);
            node->m_convolution2D = m_convolution2D;
        }
    }

    void ForwardProp(const FrameRange& fr) override
    {
        Matrix<ElemType> sliceOutputValue = ValueFor(fr);
        const Matrix<ElemType>& input0 = InputRef(0).ValueAsMatrix();
        Matrix<ElemType> sliceInput1Value = InputRef(1).ValueFor(fr);
        if (!m_transpose)
            m_convEng->Forward(sliceInput1Value, input0, sliceOutputValue, *m_tempMatrixForward);
        else
        {
            // BackwardData adds results to the output so need to zero them out first.
            // REVIEW alexeyk: should be rolled into BackwardData itself.
            sliceOutputValue.SetValue(0);
            m_convEng->BackwardData(sliceInput1Value, input0, sliceOutputValue, /*accumulateGradient =*/ true, *m_tempMatrixForward);
        }
    }

    void BackpropTo(const size_t inputIndex, const FrameRange& fr) override
    {
        auto sliceOutputGrad = GradientFor(fr);

        // this potentially computes over time, so we must mask gaps to 0
        if (Input(inputIndex)->ReducesInTimeWrt(shared_from_this()))
            MaskMissingGradientColumnsToZero(fr);
        if (Input(inputIndex)->ReducesInTimeWrt(Input(1 - inputIndex)))
            Input(1 - inputIndex)->MaskMissingValueColumnsToZero(fr);

        if (inputIndex == 0) // derivative with respect to the weight matrix
        {
            auto& grad = InputRef(0).GradientAsMatrix();
            auto sliceInput1Value = InputRef(1).ValueFor(fr);
            if (!m_transpose)
                m_convEng->BackwardKernel(sliceOutputGrad, sliceInput1Value, grad, !Input(inputIndex)->IsGradientInitializedBy(this), fr.IsAllFrames(), *m_tempMatrixBackward);
            else
                m_convEng->BackwardKernel(sliceInput1Value, sliceOutputGrad, grad, !Input(inputIndex)->IsGradientInitializedBy(this), fr.IsAllFrames(), *m_tempMatrixBackward);
        }
        else if (inputIndex == 1) // derivative with respect to the input feature
        {
            auto& input0 = InputRef(0).ValueAsMatrix();
            auto sliceInput1Grad = InputRef(1).GradientFor(fr);
            if (!m_transpose)
                m_convEng->BackwardData(sliceOutputGrad, input0, sliceInput1Grad, !Input(inputIndex)->IsGradientInitializedBy(this), *m_tempMatrixBackward);
            else
            {
                // REVIEW alexeyk: Forward overwrites values in sliceInput1Grad. Should handle correctly instead.
                m_convEng->Forward(sliceOutputGrad, input0, sliceInput1Grad, *m_tempMatrixBackward);
            }
        }
    }

    void Validate(bool isFinalValidationPass) override
    {
        Base::Validate(isFinalValidationPass);
        InferMBLayoutFromInputs(isFinalValidationPass);

        size_t inputIdx = GetExpectedNumInputs() - 1;
        TensorShape inputShape;
        TensorShape outputShape;
        // If 2D convolution syntax is used then some of the tensor dimensions need to be inferred.
        if (m_convolution2D)
        // NOTE: when m_convolution2D is true, it's a legacy branch. Code should not enter here any more.
        {
            // Need to update some tensors with correct input dims.
            auto inDims = ImageDimensions(GetInputSampleLayout(inputIdx), m_imageLayout);
            // inputShape is used in ConvolveGeometry which supports only CHW layout.
            inputShape = inDims.AsTensorShape(ImageLayoutKind::CHW);
            InferConvolution2DReductionDims(inputShape, inDims.m_numChannels);

            size_t kW = m_kernelShape[0];
            size_t kH = m_kernelShape[1];
            size_t mapCount = m_mapCount.GetNumElements();
            size_t weightCols = kW * kH * inDims.m_numChannels;

            // if mapCount is 0 then take it from the input matrix
            if (mapCount == 0)
                Input(0)->GetAsMatrixNumRows();

            // check/infer input [0] (weights)
            // BUGBUG: For now, we treat the weights as a 2D matrix. They should be a tensor proper.
            Input(0)->ValidateInferInputDimsFrom(TensorShape(mapCount, weightCols));

            if (isFinalValidationPass && (Input(0)->GetAsMatrixNumCols() != weightCols || Input(0)->GetAsMatrixNumRows() != mapCount))
            {
                LogicError("Convolution weight matrix %ls should have dimension [%d, %d] which is [outputChannels, kernelWidth * kernelHeight * inputChannels]",
                           Input(0)->NodeName().c_str(), (int)mapCount, (int)weightCols);
            }

            outputShape = this->ComputeOutputShape(inputShape, TensorShape(1), /*ceilOutDim*/false, isFinalValidationPass);
            // ConvolveGeometry always uses CHW.
            SetDims(ImageDimensions(outputShape, ImageLayoutKind::CHW).AsTensorShape(m_imageLayout), HasMBLayout());
        }
        else
        {
            inputShape = GetInputSampleLayout(inputIdx);
            // infer reduction dimensions if not given
            InferReductionDims(inputShape, inputShape);
            if (!m_transpose)
            {
                outputShape = this->ComputeOutputShape(inputShape, m_dilation, /*ceilOutDim*/false, isFinalValidationPass);

                if (m_outputShape.GetRank() > 0 && m_outputShape != TensorShape(0))    // user have explicitly set m_outputShape, we check if it's the same as outputShape
                {
                    if (m_outputShape != outputShape)
                    {
                        InvalidArgument("%ls %ls the shape of the specified convolution output %ls is different from "
                            "the result of convoluting the input argument using the provided options %ls. It is recommended "
                            "that the output shape is not specified for convolution.", NodeName().c_str(), OperationName().c_str(),
                            static_cast<std::wstring>(m_outputShape).c_str(),
                            static_cast<std::wstring>(outputShape).c_str());
                    }
                }
            }
            else
            {
                if (m_outputShape.GetRank() <= 0 || m_outputShape == TensorShape(0))
                {
                    // In case of convolution transpose (deconvolution), node input (inputShape) is really the output of the convolution
                    // and node output (outDims) is convolution input. ConvolveGeometry does not care about deconvolutions (it does not have to).
                    const size_t DEAFULT_NUM_GROUPS = 1;
                    outputShape = ConvolveGeometry::ComputeInputShape(inputShape, m_kernelShape, m_mapCount, m_stride,
                                                                      m_sharing, m_autoPad, m_lowerPad, m_upperPad, TensorShape(1), DEAFULT_NUM_GROUPS,
                                                                       false, Base::NeedsDynamicValidation(), isFinalValidationPass);
                }
                else
                {
                    // in case the user specifies the output shape, we make sure the input shape can be the result of
                    // convolution from the specified output shape
                    auto inferredShape = this->ComputeOutputShape(m_outputShape, TensorShape(1), false, isFinalValidationPass);
                    if (inputShape != inferredShape)
                        InvalidArgument("%ls %ls the shape of the convolution transpose operand %ls is different from "
                            "the result of convoluting the specified output argument using "
                            "the provided options %ls", NodeName().c_str(), OperationName().c_str(),
                            static_cast<std::wstring>(inputShape).c_str(),
                            static_cast<std::wstring>(inferredShape).c_str());
                    outputShape = m_outputShape;
                }
            }

            if (m_imageLayout == ImageLayoutKind::CHW)
                SetDims(outputShape, HasMBLayout());
            else    // legacy format
                SetDims(ImageDimensions(outputShape, ImageLayoutKind::CHW).AsTensorShape(m_imageLayout), HasMBLayout());
        }

        // update LearnableParameter if it has 0 dimensions (to be inferred)
        // Typically this would be the #inputChannels (C).
        if (Input(0)->GetSampleLayout().GetNumElements() == 0)
        {
            // BUGBUG: Inference does not support sharing. Problem is that we have the information too late.
            //         In this case, users will have to specify the correct dimensions. Good luck.
#if 1       // old style for back compat with previous results. Randomization will differ.
            if (Input(0)->GetSampleLayout().GetRank() == 2)
                Input(0)->ValidateInferInputDimsFrom(TensorShape(m_mapCount.GetNumElements(), m_kernelShape.GetNumElements()));
            else
#endif
            {
                auto weightShape = m_kernelShape.GetDims();
                for (auto outDim : m_mapCount.GetDims())
                    weightShape.push_back(outDim);
                Input(0)->ValidateInferInputDimsFrom(TensorShape(weightShape));
            }
        }

        if (isFinalValidationPass)
        {
            bool recomputeConvGeometry = (m_convEng == nullptr) ? false : // For first minibatch, this flag must be false, so initial mem allocation can happen.
                                          (outputShape != m_convEng->Geometry()->OutputShape()) || (inputShape != m_convEng->Geometry()->InputShape());
            if ((m_convEng == nullptr) || ((m_convEng != nullptr) && recomputeConvGeometry))
            {
                auto geometry = std::make_shared<ConvolveGeometry>(!m_transpose ? inputShape : outputShape,
                                                                   m_kernelShape, m_mapCount, m_stride,
                                                                   m_sharing, m_autoPad, m_lowerPad, m_upperPad, m_dilation, false, m_groups);
                m_convEng = ConvolutionEngine<ElemType>::Create(geometry, m_deviceId, m_imageLayout,
                                                                m_maxTempMemSizeInSamples, m_poolKind,
                                                                ConvolutionEngineKind::All, NodeName(), Globals::ShouldForceDeterministicAlgorithms(),
                                                                false, recomputeConvGeometry);
            }

            if (Input(0)->GetSampleLayout().GetNumElements() != m_kernelShape.GetNumElements() * m_convEng->Geometry()->KernelCount())
            {
                LogicError("Convolution weight matrix %ls should have dimension [(filter shape) x (input channels) x (output channels)]",
                           Input(0)->NodeName().c_str());
            }
        }
    }

    void RequestMatricesBeforeForwardProp(MatrixPool& matrixPool) override
    {
        Base::RequestMatricesBeforeForwardProp(matrixPool);
        RequestMatrixFromPool(m_tempMatrixForward, matrixPool, 0, false, true);
    }

    // m_tempMatrixForward is only used as workspace for convolution, we can release it immediately afterwards
    void ReleaseMatricesAfterForwardProp(MatrixPool& matrixPool) override
    {
        Base::ReleaseMatricesAfterForwardProp(matrixPool);
        ReleaseMatrixToPool(m_tempMatrixForward, matrixPool);
    }

    void RequestMatricesBeforeBackprop(MatrixPool& matrixPool) override
    {
        Base::RequestMatricesBeforeBackprop(matrixPool);
        RequestMatrixFromPool(m_tempMatrixBackward, matrixPool, 0, false, true);
    }

    void ReleaseMatricesAfterBackprop(MatrixPool& matrixPool) override
    {
        Base::ReleaseMatricesAfterBackprop(matrixPool);
        ReleaseMatrixToPool(m_tempMatrixBackward, matrixPool);
    }

    void SetmMaxTempMemSizeInSamples(const size_t maxTempMemSizeInSamples)
    {
        m_maxTempMemSizeInSamples = maxTempMemSizeInSamples;
        if (m_convEng != nullptr)
            m_convEng->SetmMaxTempMemSizeInSamples(maxTempMemSizeInSamples);
    }

    bool IsConvolution2D() const { return m_convolution2D; }

    bool OutputUsedInComputingInputNodesGradients() const override { return false; }

protected:
    virtual void /*ConvolutionNode::*/InferMBLayoutFromInputs(bool isFinalValidationPass)
    {
        InferMBLayoutFromInputsForStandardCase(isFinalValidationPass);
    }

private:
    using TransformerNode::m_transforms;
    using ConvolutionNodeBase<ElemType>::ComputeFilterTransform;

    virtual void /*TransformerNode::*/ComputeTransforms() override
    {
        if (m_transforms[1].m_axisTransforms.empty())
        {
            m_transforms[1] = ComputeFilterTransform();
            if (!m_transpose)
            {
                // Convolution, need to inverse transform.
                m_transforms[1] = m_transforms[1].Inverse();
            }
            // else: Deconvolution, nothing to do.
        }
        // else: transform already computed, no need to do computation again.
    }

    virtual bool /*TransformerNode::*/SupportsTransformOnInput(size_t inputIndex) override
    {
        // We support transforms just on convolution input.
        return (inputIndex == 1);
    }

    virtual TensorShape /*ConvolutionNode::*/ComputeOutputShape(const TensorShape& inputShape,
        const TensorShape& dilate, bool ceilOutDim, bool isFinalValidationPass)
    {
        return ConvolveGeometry::ComputeOutputShape(inputShape, m_kernelShape, m_mapCount, m_stride,
            m_sharing, m_autoPad, m_lowerPad, m_upperPad, dilate, m_groups, ceilOutDim,
            Base::NeedsDynamicValidation(), isFinalValidationPass);
    }

    TensorShape m_dilation;
    size_t m_groups;

protected:
    // Flag that indicates whether the node is created using 2D-syntax.
    bool m_convolution2D;
};


// -----------------------------------------------------------------------
// ConvolutionOverSequenceAxisNode -- for convolution over sequence axis
//
// The purpose of this node is to extend the original convolution node to also output
// the correct sequence size for every output sequences.
//
// The input features variable(Input1) has already been converted such that the original sequence axis is
// unpacked to static axis. The size of different original sequences varies, so the unpacked
// sequence axis has dim as the longest sequence size while shorter sequences have their values padded with 0.
// Thus in order to restore the output sequence axis after convolution, we compute the correct size for output sequences here as a seperate output.
// Both outputs will serve as inputs to ToSequence(convResult, seqSizes), converting sequence axes back with correct sizes. 
//
// Input0: kernels
// Input1: Input features           [i_0 x i_1 x ... x i_n x max(S) x C]    x [1(*) x #]
// Input2: Input sequence size      [1]                                     x [1(*) x #]
// output0: Conv output             [o_0 x o_1 x ... x o_n x O(max(S)) x D] x [1(*) x #]
// output1: output sequence size    [1]                                     x [1(*) x #]
//
// where i_k = dim of k-th input feature, o_k = dim of k-th output feature,
//       O() = output dim given input dim and other convolution attributes, S = set of originial sequence sizes,
//       C = input channel dim, D = output depth dim, # = batch size, * denotes the sequence axis.
// -----------------------------------------------------------------------
template <class ElemType>
class ConvolutionOverSequenceAxisNode : public ConvolutionNode<ElemType>, public NumInputs<3>, public MultiOutputNode<ElemType>
{
    typedef ConvolutionNode<ElemType> Base; UsingConvolutionNodeBaseMembers;
private:
    using Base::m_dilation;
    using Base::m_groups;

    static const size_t SentinelUnspecifiedSeqAxisIdx = (size_t) -1;

    enum InputIndices : size_t
    {
        InputConvolutionMapIdx = 0,
        InputOperandIdx = 1,
        InputSeqAxisDimIdx = 2,
    };

    enum OutputIndices : size_t
    {
        OutputOperandIdx = 0,
        OutputSeqAxisDimIdx = 1,
    };

public:
    static const std::wstring TypeName() { return L"ConvolutionOverSequence"; }

public:
    ConvolutionOverSequenceAxisNode(DEVICEID_TYPE deviceId, const wstring& name)
        : Base(deviceId, name), MultiOutputNode<ElemType>(2), m_seqAxisIdx(SentinelUnspecifiedSeqAxisIdx)
    {
    }

    ConvolutionOverSequenceAxisNode(DEVICEID_TYPE deviceId, const wstring& name, const TensorShape& kernelShape, const TensorShape& mapCount, const TensorShape& strideShape,
                                    const std::vector<bool>& sharing, const std::vector<bool>& autoPadding, const TensorShape& lowerPad, const TensorShape& upperPad,
                                    bool transpose, const TensorShape& outputShape, ImageLayoutKind imageLayout, size_t maxTempMemSizeInSamples, const TensorShape& dilation = TensorShape(1),
                                    size_t groups = 1)
        : Base(deviceId, name, kernelShape, mapCount, strideShape, sharing, autoPadding, lowerPad, upperPad, transpose, outputShape, imageLayout, maxTempMemSizeInSamples, dilation, groups),
          MultiOutputNode<ElemType>(2),
          m_seqAxisIdx(SentinelUnspecifiedSeqAxisIdx)
    {
    }

    ConvolutionOverSequenceAxisNode(const ScriptableObjects::IConfigRecordPtr configp)
        : Base(configp), MultiOutputNode<ElemType>(2), m_seqAxisIdx(SentinelUnspecifiedSeqAxisIdx)
    {
    }

public:
    void ForwardProp(const FrameRange& fr) override
    {
#define _PERF_CONV 0
#if _PERF_CONV
        auto duration_conv = std::chrono::duration<float>(0);
        auto duration_size = std::chrono::duration<float>(0);

        auto duration_size_pre = std::chrono::duration<float>(0);
        auto duration_size_attr = std::chrono::duration<float>(0);
        auto duration_size_value = std::chrono::duration<float>(0);
        auto duration_size_tensor = std::chrono::duration<float>(0);

        auto conv_start = std::chrono::system_clock::now();
#endif
        Base::ForwardProp(fr);
#if _PERF_CONV
        duration_conv += (std::chrono::system_clock::now() - conv_start);

        auto size_start = std::chrono::system_clock::now();
        auto size_pre_start = std::chrono::system_clock::now();
#endif

        const size_t inputOperandIdx = InputIndices::InputOperandIdx;
        const size_t inputSeqAxisDimIdx = InputIndices::InputSeqAxisDimIdx;
        const size_t outputSeqAxisDimIdx = OutputIndices::OutputSeqAxisDimIdx;

        auto& inputSeqAxisDimValue = Input(inputSeqAxisDimIdx)->Value();
        auto& outputSeqAxisDimValue = *m_outputsValue[outputSeqAxisDimIdx];
        outputSeqAxisDimValue.SetValue(inputSeqAxisDimValue);
#if _PERF_CONV
        duration_size_pre += (std::chrono::system_clock::now() - size_pre_start);
#endif
        if (!m_transpose)
        {
#if _PERF_CONV
            auto size_attr_start = std::chrono::system_clock::now();
#endif
            // TODO: add shape check here. 
            // TODO: add matrix storage assumption check here (dense vs sparse etc).

            // Same computing logic in ConvolveGeometry.
            // Rewritten here because TensorShape uses SmallVector which cannot exceed size of 12, a size too small for sequence count.
            size_t kernelShape_i = m_kernelShape[m_seqAxisIdx];
            size_t delta = m_stride[m_seqAxisIdx];
            size_t dil = m_dilation[m_seqAxisIdx];
            bool autoPadCur = m_autoPad[m_seqAxisIdx];
            size_t lo = m_lowerPad[m_seqAxisIdx];
            size_t hi = m_upperPad[m_seqAxisIdx];
            size_t effectiveKernelShape = (kernelShape_i - 1) * dil + 1;
#if _PERF_CONV
            duration_size_attr += (std::chrono::system_clock::now() - size_attr_start);
            auto size_value_start = std::chrono::system_clock::now();
#endif
            if (autoPadCur)
            {
                outputSeqAxisDimValue += (ElemType) dil * (kernelShape_i - 1);
            }
            else
            {
                outputSeqAxisDimValue += (ElemType) lo + hi;
            }

            //outputSeqAxisDimValue = (outputSeqAxisDimValue - (ElemType) effectiveKernelShape) / (ElemType) delta + (ElemType) 1;
            // test theory
            outputSeqAxisDimValue -= (ElemType) effectiveKernelShape;
            outputSeqAxisDimValue /= (ElemType) delta;
            outputSeqAxisDimValue += 1;
#if _PERF_CONV
            duration_size_value += (std::chrono::system_clock::now() - size_value_start);
            auto size_tensor_start = std::chrono::system_clock::now();
#endif
            TensorShape outputSeqAxisDimTensorShape{outputSeqAxisDimValue.GetNumRows(), outputSeqAxisDimValue.GetNumCols()};
            if (outputSeqAxisDimTensorShape.GetDim(0) != 1)
            {
                RuntimeError("%ls %ls sequential convolution should output sequence sizes of shape [1 x #sequences], instead of [%d x #sequences]",
                    NodeName().c_str(), OperationName().c_str(), outputSeqAxisDimTensorShape.GetDim(0));
            }

            TensorView<ElemType> outputSeqAxisDimTensorView = TensorView<ElemType>(
                std::make_shared<Matrix<ElemType>>(outputSeqAxisDimValue.AsReference()), outputSeqAxisDimTensorShape);
            outputSeqAxisDimTensorView.DoUnaryOpOf(0, outputSeqAxisDimTensorView, 1, opFloor, opSum);
#if _PERF_CONV
            duration_size_tensor += (std::chrono::system_clock::now() - size_tensor_start);
#endif
        }
        else
        {
            InvalidArgument("%ls %ls convolution over sequence axis currently does not support transpose. ",
                NodeName().c_str(), OperationName().c_str());
        }
#if _PERF_CONV
        duration_size += std::chrono::system_clock::now() - size_start;

        fprintf(stderr, "forward convolution: %07fs, seq size: %07fs\nPre: %09fs, attr: %09fs, val: %09fs, tensor: %09fs\n", 
            duration_conv.count(), duration_size.count(),
            duration_size_pre.count(), duration_size_attr.count(), duration_size_value.count(), duration_size_tensor.count());
#endif
    }

    void BackpropTo(const size_t inputIndex, const FrameRange& fr) override
    {
        // We don't need backprop for the 3rd input, i.e. the sequence sizes.
        const size_t inputSeqAxisDimIdx = InputIndices::InputSeqAxisDimIdx;
        if (inputIndex < inputSeqAxisDimIdx)
            Base::BackpropTo(inputIndex, fr);
    }

    void Validate(bool isFinalValidationPass) override
    {
        Base::Validate(isFinalValidationPass);

        const size_t inputSeqAxisDimIdx = InputIndices::InputSeqAxisDimIdx;
        const size_t outputSeqAxisDimIdx = OutputIndices::OutputSeqAxisDimIdx;

        // update conv output seq dim shape.
        TensorShape outputShape = GetInputSampleLayout(inputSeqAxisDimIdx);

        if (m_outputsShape[outputSeqAxisDimIdx].GetRank() > 0 && m_outputsShape[outputSeqAxisDimIdx] != TensorShape(0))
        {
            if (m_outputsShape[outputSeqAxisDimIdx] != outputShape)
            {
                InvalidArgument("%ls %ls the shape of the specified convolution out sequence axis dim %ls is different from the result using provided options %ls",
                    NodeName().c_str(), OperationName().c_str(), static_cast<std::wstring>(m_outputsShape[outputSeqAxisDimIdx]).c_str(), static_cast<std::wstring>(outputShape).c_str());
            }
        }

        this->m_outputsMBLayout[outputSeqAxisDimIdx] = GetMBLayout();
        this->m_outputsShape[outputSeqAxisDimIdx] = outputShape;

        if (isFinalValidationPass)
        {
            const size_t inputOperandIdx = InputIndices::InputOperandIdx;
            // Input1: Input features           [i_1 x i_2 x ... x i_n x max(S) x C]    x [1(*) x #]
            // The input sample layout rank should always >= 2.
            size_t inputOperandRank = GetInputSampleLayout(inputOperandIdx).GetRank();
            m_seqAxisIdx = inputOperandRank - 2;
            if (inputOperandRank < 2)
                InvalidArgument("%ls %ls the input sample layout rank for sequential convolution should always >= 2. The provided input has sample layout rank %d",
                                NodeName().c_str(), OperationName().c_str(), inputOperandRank);
        }
    }

private:
    size_t m_seqAxisIdx;
};

// -----------------------------------------------------------------------
//
//
//
// -----------------------------------------------------------------------
template <class ElemType>
class ConvolutionOverSequenceAxisNodeV2 : public ConvolutionNode<ElemType>
{
    typedef ConvolutionNode<ElemType> Base; UsingConvolutionNodeBaseMembers;
private:
    using Base::m_dilation;
    using Base::m_groups;

    static const size_t SentinelUnspecifiedSeqAxisIdx = (size_t)-1;
    static const size_t OperandInputIdx = 1;

public:
    static const std::wstring TypeName() { return L"ConvolutionOverSequenceV2"; }

public:
    ConvolutionOverSequenceAxisNodeV2(DEVICEID_TYPE deviceId, const wstring& name)
        : Base(deviceId, name), m_seqAxisIdx(SentinelUnspecifiedSeqAxisIdx), m_isGradientUnpackedAndTransposed(false)
    {
    }

    ConvolutionOverSequenceAxisNodeV2(DEVICEID_TYPE deviceId, const wstring& name, const TensorShape& kernelShape, const TensorShape& mapCount, const TensorShape& strideShape,
        const std::vector<bool>& sharing, const std::vector<bool>& autoPadding, const TensorShape& lowerPad, const TensorShape& upperPad,
        bool transpose, const TensorShape& outputShape, ImageLayoutKind imageLayout, size_t maxTempMemSizeInSamples, const TensorShape& dilation = TensorShape(1),
        size_t groups = 1)
        : Base(deviceId, name, kernelShape, mapCount, strideShape, sharing, autoPadding, lowerPad, upperPad, transpose, outputShape, imageLayout, maxTempMemSizeInSamples, dilation, groups),
        m_seqAxisIdx(SentinelUnspecifiedSeqAxisIdx),
        m_isGradientUnpackedAndTransposed(false)
    {
    }

    ConvolutionOverSequenceAxisNodeV2(const ScriptableObjects::IConfigRecordPtr configp)
        : Base(configp), m_seqAxisIdx(SentinelUnspecifiedSeqAxisIdx), m_isGradientUnpackedAndTransposed(false)
    {
    }

public:
    void Save(File& fstream) const override
    {
        // TODO
    }

    void Load(File& fstream, size_t modelVersion) override
    {
        // TODO
    }

    void CopyTo(ComputationNodeBasePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const override
    {
        // TODO
    }

    void ForwardProp(const FrameRange& fr) override
    {
        // TODO: I think we need to check if fr is happening for fullframerange here.
        //       current implementation uses unpack and pack, which is nonlooping. 
        // TODO: optimized version of Unpack Forward + Base Forward + ToSequence Forward should happen here.
        m_isGradientUnpackedAndTransposed = false;
#define _CONV_SEQ_DEBUG 0
#if _CONV_SEQ_DEBUG
        fprintf(stderr, "%ls %ls forward value shape (%zu, %zu), layout num columns %zu\n",
            NodeName().c_str(), OperationName().c_str(), InputRef(1).Value().GetNumRows(), InputRef(1).Value().GetNumCols(),
            InputRef(1).GetMBLayout()->GetNumCols());
#endif
#define _PERF_CONV_2 1
#if _PERF_CONV_2
#define _PERF_INIT(tag)                             \
    std::vector<float> _durationVec;\
    std::vector<std::string> _durationStr;\
    auto _duration = std::chrono::duration<float>(0);\
    auto _start = std::chrono::system_clock::now();\
    _durationStr.push_back(#tag)
#define _PERF_SET_AND_INIT(tag)                     \
    _duration += (std::chrono::system_clock::now() - _start); \
    _durationVec.push_back(_duration.count());\
    _durationStr.push_back(#tag);\
    _duration = std::chrono::duration<float>(0);\
    _start = std::chrono::system_clock::now()
#define _PERF_CONV_PHASE "Forward"
#define _PERF_SET \
    _duration += (std::chrono::system_clock::now() - _start);\
    _durationVec.push_back(_duration.count());\
    fprintf(stderr, "Profiling seq conv v2 "_PERF_CONV_PHASE": ");\
    for (size_t i=0; i<_durationVec.size(); ++i)\
    {\
        fprintf(stderr, "task: %s time: %07fs; ", _durationStr[i].c_str(), _durationVec[i]);\
    }\
    fprintf(stderr, "\n")
#endif

#if _PERF_CONV_2
        _PERF_INIT(unpack_input);
#endif
        /// Unpack input operand
        const size_t inputIdx = 1;

        auto inputMBLayout = InputRef(inputIdx).GetMBLayout();
        if (inputMBLayout->HasSequenceBeyondBegin() || inputMBLayout->HasSequenceBeyondEnd())
            LogicError("%ls: %s truncated sequence not supported.", Base::NodeDescription().c_str(), typeid(*this).name());

        // create output MBLayout. one parallel sequence per sample.
        //auto intermediateLayout = std::make_shared<MBLayout>();
        m_unpackedpMBLayout->InitAsFrameMode(inputMBLayout->GetNumSequences());

        // TODO : Here unpack use this function to update output value matrix size.
        //        Problem is they the sampleLayout seems wrong as you don't have the sequence static axis dim. (or do they managed to have that at validate?). 
        //        Update: confirmed, sample layout is updated at validate. 
        // TODO : member function uses m_pMBLayout. we need to rewrite the whole thing and duplicate logic. can we not perform this op?
        // UpdateFunctionValuesSize();
        UpdateFunctionValuesSize(*m_unpackedOperandData, m_unpackedOperandShape, m_unpackedpMBLayout);

        //auto unpackedMatrixNumRows = m_unpackedOperandData->GetNumRows();
        //auto unpackedMatrixNumCols = m_unpackedOperandData->GetNumCols();
        ElemType paddingValue = 0.0;
        TensorView<ElemType> unpackedInput = ComputationNode<ElemType>::Unpack(InputRef(1).GetSampleLayout(), InputRef(1).Value(), InputRef(1).GetMBLayout(), 
            m_unpackedOperandData, m_tempScatterIndices, m_tempMask, /*batchMajor=*/ false, &paddingValue);
        //if (unpackedInput.GetSOBPtr() != m_unpackedOperandData)
        //    m_unpackedOperandData->AssignValuesOf(*unpackedInput.GetSOBPtr());
        //m_unpackedOperandData->Reshape(unpackedMatrixNumRows, unpackedMatrixNumCols);

#if _PERF_CONV_2
        _PERF_SET_AND_INIT(transpose_unpack_input);
#endif

#if _CONV_SEQ_DEBUG
        fprintf(stderr, "%ls %ls forward value shape (%zu, %zu), layout num columns %zu\n",
            NodeName().c_str(), OperationName().c_str(), InputRef(1).Value().GetNumRows(), InputRef(1).Value().GetNumCols(),
            InputRef(1).GetMBLayout()->GetNumCols());
#endif

        TensorShape transposedUnpackedOperandShape = m_unpackedOperandShape;
        size_t transposedUnpackedOperandShapeRank = transposedUnpackedOperandShape.GetRank();
        transposedUnpackedOperandShape.AppendInPlace(transposedUnpackedOperandShapeRank++, m_unpackedpMBLayout->GetNumParallelSequences());
        transposedUnpackedOperandShape.AppendInPlace(transposedUnpackedOperandShapeRank++, m_unpackedpMBLayout->GetNumTimeSteps());
        const size_t inputRank = GetInputSampleLayout(inputIdx).GetRank();
        transposedUnpackedOperandShape.SwapDimsInPlace(inputRank - 1, inputRank);

        UpdateFunctionValuesSize(*m_transposedUnpackedOperandData, m_unpackedOperandShape, m_unpackedpMBLayout);

        unpackedInput = unpackedInput.Reshaped(transposedUnpackedOperandShape);
        TensorView<ElemType> transposedUnpackedInput(m_transposedUnpackedOperandData, transposedUnpackedOperandShape.GetDims());
        transposedUnpackedInput.AssignCopyOf(unpackedInput);

#if _PERF_CONV_2
        _PERF_SET_AND_INIT(compute_output_seq_length);
#endif

#if _CONV_SEQ_DEBUG
        fprintf(stderr, "%ls %ls forward value shape (%zu, %zu), layout num columns %zu\n",
            NodeName().c_str(), OperationName().c_str(), InputRef(1).Value().GetNumRows(), InputRef(1).Value().GetNumCols(),
            InputRef(1).GetMBLayout()->GetNumCols());
#endif

        // transpose
        // auto transposedUnpackedOperandData = TensorView<ElemType>(m_unpackedOperandData, m_unpackedOperandShape);

        // Get original sequence lengths
        auto numSequences = inputMBLayout->GetNumSequences();
        m_inputSequenceLengths = std::vector<size_t>(numSequences, 0);
        let& inputSequences = inputMBLayout->GetAllSequences();
        size_t j = 0;
        for (size_t i = 0; i < inputSequences.size(); ++i)
        {
            let& seq = inputSequences[i];
            if (seq.seqId == GAP_SEQUENCE_ID)
                continue;

            m_inputSequenceLengths[j] = seq.GetNumTimeSteps();
            j++;
        }
        assert(j == numSequences);

        std::vector<size_t> convResultSequenceLengths = GetOutputSequenceLengths(m_inputSequenceLengths);

#if _PERF_CONV_2
        _PERF_SET_AND_INIT(forward);
#endif

        /// Forward
        UpdateFunctionValuesSize(*m_unpackedConvResultData, m_unpackedConvResultShape, m_unpackedpMBLayout);

        Matrix<ElemType> sliceOutputValue = m_unpackedConvResultData->AsReference();
        const Matrix<ElemType>& input0 = InputRef(0).ValueAsMatrix();
        Matrix<ElemType> sliceInput1Value = m_transposedUnpackedOperandData->AsReference();

        m_convEng->Forward(sliceInput1Value, input0, sliceOutputValue, *m_tempMatrixForward);

#if _CONV_SEQ_DEBUG
        fprintf(stderr, "%ls %ls forward value shape (%zu, %zu), layout num columns %zu\n",
            NodeName().c_str(), OperationName().c_str(), InputRef(1).Value().GetNumRows(), InputRef(1).Value().GetNumCols(),
            InputRef(1).GetMBLayout()->GetNumCols());
#endif
#if _PERF_CONV_2
        _PERF_SET_AND_INIT(transpose_output);
#endif
        // transpose output
        UpdateFunctionValuesSize(*m_transposedUnpackedConvResultData, m_unpackedConvResultShape, m_unpackedpMBLayout);

        TensorShape transposedUnpackedConvResultShape = m_unpackedConvResultShape;
        size_t transposedUnpackedConvResultShapeRank = transposedUnpackedConvResultShape.GetRank();
        transposedUnpackedConvResultShape.AppendInPlace(transposedUnpackedConvResultShapeRank++, m_unpackedpMBLayout->GetNumParallelSequences());
        transposedUnpackedConvResultShape.AppendInPlace(transposedUnpackedConvResultShapeRank++, m_unpackedpMBLayout->GetNumTimeSteps());
        transposedUnpackedConvResultShape.SwapDimsInPlace(inputRank - 1, inputRank);
        
        TensorView<ElemType> unpackedConvResult(m_unpackedConvResultData, transposedUnpackedConvResultShape);
        TensorView<ElemType> transposedUnpackedConvResult(m_transposedUnpackedConvResultData, transposedUnpackedConvResultShape.GetDims());
        transposedUnpackedConvResult.AssignCopyOf(unpackedConvResult);

#if _CONV_SEQ_DEBUG
        fprintf(stderr, "%ls %ls forward value shape (%zu, %zu), layout num columns %zu\n",
            NodeName().c_str(), OperationName().c_str(), InputRef(1).Value().GetNumRows(), InputRef(1).Value().GetNumCols(),
            InputRef(1).GetMBLayout()->GetNumCols());
#endif
#if _PERF_CONV_2
        _PERF_SET_AND_INIT(tosequence_transposed_output_1);
#endif

        /// tosequence sliceOutputValue
        m_transposedUnpackedConvResultShape.AppendInPlace(m_transposedUnpackedConvResultShape.GetRank(), numSequences);
        auto unpackedConvResultDataNDArrayView = ::CNTK::MakeSharedObject<::CNTK::NDArrayView>(::CNTK::AsDataType<ElemType>(),
            ::CNTK::AsDeviceDescriptor(m_transposedUnpackedConvResultData->GetDeviceId()),
            ::CNTK::AsStorageFormat(m_transposedUnpackedConvResultData->GetFormat()),
            ::CNTK::AsNDShape(m_transposedUnpackedConvResultShape),
            /*readOnly =*/ true,
            new TensorView<ElemType>(m_transposedUnpackedConvResultData, m_transposedUnpackedConvResultShape));

#if _PERF_CONV_2
        _PERF_SET_AND_INIT(tosequence_transposed_output_2);
#endif

        auto convResultDataValue = ::CNTK::MakeSharedObject<::CNTK::Value>(unpackedConvResultDataNDArrayView, ::CNTK::CreateMask(convResultSequenceLengths));

#if _PERF_CONV_2
        _PERF_SET_AND_INIT(tosequence_transposed_output_3);
#endif

        auto dummyVar = ::CNTK::InputVariable(::CNTK::AsNDShape(GetSampleLayout()), this->IsValueSparse(), ::CNTK::AsDataType<ElemType>());
#ifdef _MSC_VER
        auto& outputValuePtrRef = ValuePtrRef();
#else
        auto& outputValuePtrRef = this->template ValuePtrRef();
#endif
#if _CONV_SEQ_DEBUG
        fprintf(stderr, "%ls %ls forward value shape (%zu, %zu), layout num columns %zu\n",
            NodeName().c_str(), OperationName().c_str(), InputRef(1).Value().GetNumRows(), InputRef(1).Value().GetNumCols(),
            InputRef(1).GetMBLayout()->GetNumCols());
#endif
        auto packedMatrixAndLayout = ::CNTK::Utils::GetCNTKImplMatrixAndMBLayoutFromValueObject(dummyVar, convResultDataValue, nullptr, outputValuePtrRef, m_tempGatherIndices);

#if _PERF_CONV_2
        _PERF_SET_AND_INIT(tosequence_transposed_output_4);
#endif

        let& outMBLayout = GetMBLayout();
        outMBLayout->CopyFrom(packedMatrixAndLayout.second, /*keepName=*/ true);
        // Previously, mblayout are linked, that is, as ptr and shared across many nodes: layout should stay the same for all nodes. 
        // Here we need to generate new layout. 
        // Update: we can't generate new layout here as it is too late: other nodes may already linked to our layout here.
        //         thus we must generate new layout at validation step. 
#if _PERF_CONV_2
        _PERF_SET_AND_INIT(tosequence_transposed_output_5);
#endif
        if (packedMatrixAndLayout.first != outputValuePtrRef)
            Value().AssignValuesOf(*packedMatrixAndLayout.first);

#if _PERF_CONV_2
        _PERF_SET;
#endif

#if _CONV_SEQ_DEBUG
        fprintf(stderr, "%ls %ls forward final value shape (%zu, %zu) outMBLayout has num cols (%zu). And input has value shape (%zu, %zu) with layout num cols (%zu)\n",
            NodeName().c_str(), OperationName().c_str(), Value().GetNumRows(), Value().GetNumCols(), outMBLayout->GetNumCols(),
            InputRef(1).Value().GetNumRows(), InputRef(1).Value().GetNumCols(), InputRef(1).GetMBLayout()->GetNumCols());
#endif
    }

    void BackpropTo(const size_t inputIndex, const FrameRange& fr) override
    {
        // TODO: optimized version of ToSequence Backward + Base Backward + Unpack Backward should happen here. 
        /*
        auto sliceOutputGrad = GradientFor(fr);

        // this potentially computes over time, so we must mask gaps to 0
        if (Input(inputIndex)->ReducesInTimeWrt(shared_from_this()))
            MaskMissingGradientColumnsToZero(fr);
        if (Input(inputIndex)->ReducesInTimeWrt(Input(1 - inputIndex)))
            Input(1 - inputIndex)->MaskMissingValueColumnsToZero(fr);

        if (inputIndex == 0) // derivative with respect to the weight matrix
        {
            auto& grad = InputRef(0).GradientAsMatrix();
            auto sliceInput1Value = InputRef(1).ValueFor(fr);
            m_convEng->BackwardKernel(sliceOutputGrad, sliceInput1Value, grad, !Input(inputIndex)->IsGradientInitializedBy(this), fr.IsAllFrames(), *m_tempMatrixBackward);
        }
        else if (inputIndex == 1) // derivative with respect to the input feature
        {
            auto& input0 = InputRef(0).ValueAsMatrix();
            auto sliceInput1Grad = InputRef(1).GradientFor(fr);
            m_convEng->BackwardData(sliceOutputGrad, input0, sliceInput1Grad, !Input(inputIndex)->IsGradientInitializedBy(this), *m_tempMatrixBackward);
        }
        */
#if _CONV_SEQ_DEBUG
        fprintf(stderr, "%ls %ls backward gradient value shape (%zu, %zu)\n",
            NodeName().c_str(), OperationName().c_str(), Gradient().GetNumRows(), Gradient().GetNumCols());
#endif
#if _PERF_CONV_2
#undef _PERF_CONV_PHASE
#define _PERF_CONV_PHASE "Backward"
#endif

        // this potentially computes over time, so we must mask gaps to 0
        // TODO : Huge buggy potential here. Not sure if this is needed, as in sequential conv cases there shouldn't be 'gaps' for the conv operation,
        //        as we are convolving over the constructed layout, where every sequence has length only 1, and the orignial gaps if any are all padded with 0. 
        //        On top of that, the gradients in those positions should be masked away when we transform the gradient shape back and assign to input gradient. 
        //if (Input(inputIndex)->ReducesInTimeWrt(shared_from_this()))
        //    MaskMissingGradientColumnsToZero(fr);
        //if (Input(inputIndex)->ReducesInTimeWrt(Input(1 - inputIndex)))
        //    Input(1 - inputIndex)->MaskMissingValueColumnsToZero(fr);

        // Unpack and transpose Gradient(). We might need to store it somewhere as we need it for both input index. use a m_gradientUnpacked?
#if _PERF_CONV_2
        _PERF_INIT(unpack_and_transpose_gradient);
#endif

        if (!m_isGradientUnpackedAndTransposed)
        {
            UnpackAndTransposeGradient();
            m_isGradientUnpackedAndTransposed = true;
        }

        auto unpackedOutputGrad = m_transposedUnpackedGrad->AsReference();

        if (inputIndex == 0)
        {
#if _PERF_CONV_2
            _PERF_SET_AND_INIT(kernel_backward);
#endif

            // kernel gradient
            auto& grad = InputRef(0).GradientAsMatrix();
            auto unpackedOperandValue = m_transposedUnpackedOperandData->AsReference();
            // TODO : watch out for 1. IsGradientInitializedBy(this) 2. fr.IsAllFrames().
            //          1. is 'this' safe and correct?
            //          2. in forward fr.IsAllFrames() is always true, not sure for backward. And if it is always true, why bother with this flag anyway??
            // Update: Ok... checked ConvolutionEngine.cpp.     fr.IsAllFrames()    AKA allowReuse is used in legacy.
            //                                                  IsGradInitBy(this)  AKA accumulateGradient is used in MKL and cudnn.
            m_convEng->BackwardKernel(unpackedOutputGrad, unpackedOperandValue, grad, !Input(inputIndex)->IsGradientInitializedBy(this), fr.IsAllFrames(), *m_tempMatrixBackward);
        }
        else
        {
#if _PERF_CONV_2
            _PERF_SET_AND_INIT(feature_backward);
#endif
            // feature gradient
            auto& input0 = InputRef(0).ValueAsMatrix();
            UpdateFunctionValuesSize(*m_unpackedOperandGrad, m_unpackedOperandShape, m_unpackedpMBLayout);
            auto unpackedOperandGrad = m_unpackedOperandGrad->AsReference();
            m_convEng->BackwardData(unpackedOutputGrad, input0, unpackedOperandGrad, /*accumulateGradient*/false, *m_tempMatrixBackward);

#if _PERF_CONV_2
            _PERF_SET_AND_INIT(transpose_backward_gradient);
#endif

            // transpose gradient, now gradient should have input unpacked transposed shape.
            // need to transpose to input unpacked shape. 
            TransposeSequenceChannelAxisWithTransposedDims(m_unpackedOperandGrad, m_transposedUnpackedOperandGrad, m_unpackedOperandShape.GetDims(), m_unpackedpMBLayout);

#if _PERF_CONV_2
            _PERF_SET_AND_INIT(tosequence_backward_gradient);
#endif
            // to sequence gradient. 
            ToSequenceGradient();

#if _CONV_SEQ_DEBUG
            fprintf(stderr, "%ls %ls backward final gradient value shape (%zu, %zu)\n",
                NodeName().c_str(), OperationName().c_str(), InputRef(1).Gradient().GetNumRows(), InputRef(1).Gradient().GetNumCols());
#endif
        }
#if _PERF_CONV_2
        _PERF_SET;
#endif

    }

    // Note: for sequence lengths. 
    bool ForceDynamicValidation() const override { return true; }

    void Validate(bool isFinalValidationPass) override
    {
        // TODO: we can't use Base(ConvolutionNode) validate as we shouldn't feed seq conv input to base right?
        //       must write new version of validate. 
        ConvolutionNodeBase<ElemType>::Validate(isFinalValidationPass);
        // TODO : check if we need this dense vs sparse. 
        // ComputationNodeBase::m_isValueSparse = Input(1)->IsValueSparse();
        
        // We need to initialize the following things before Forward. 
        //     m_unpackedOperandData. we need to setup the correct shape. 
        //     All other setups in ConvolutionNode::Validate() 
        //     All other setups in UnpackSequenceNode::Validate()
        //     All other setups in ToSequence::Validate()

        // UnpackSequenceNode::Validate
        // Prepare MBLayout and Shape for intermediate unpack result. 
        ValidateUnpack(isFinalValidationPass);

        // ConvolutionNode::Validate
        // Need to insert seq axis into input shape. Run ConvolutionNode::Validate with emulated inputOperandShape. 
        ValidateConvolution(isFinalValidationPass);

        // ToSequenceNode::Validate
        ValidateToSequence(isFinalValidationPass);

        if (isFinalValidationPass)
        {
            const size_t inputIdx = GetExpectedNumInputs() - 1;
            // Input1: Input features           [i_1 x i_2 x ... x i_n x C         ]    x [S(*) x #]
            // Emulated Input:                  [i_1 x i_2 x ... x i_n x max(S) x C]    x [1(*) x #]
            // The input sample layout rank should always >= 2.
            size_t inputOperandRank = GetInputSampleLayout(inputIdx).GetRank();
            // setup emulated static axis index for sequence axis. i.e. index of (max(S)) in above example. 
            m_seqAxisIdx = inputOperandRank - 1;
            if (inputOperandRank < 1)
                InvalidArgument("%ls %ls the input sample layout rank for sequential convolution should always >= 1. The provided input has sample layout rank %d",
                    NodeName().c_str(), OperationName().c_str(), inputOperandRank);
        }
    }

    void RequestMatricesBeforeForwardProp(MatrixPool& matrixPool) override
    {
        Base::RequestMatricesBeforeForwardProp(matrixPool);
        RequestMatrixFromPool(m_tempGatherIndices, matrixPool, 1, true);
        RequestMatrixFromPool(m_tempScatterIndices, matrixPool, 1, true);
        m_tempMask = std::make_shared<Matrix<char>>(Base::m_deviceId); // why? => Matrix<char> not in matrixPool ... 
        const size_t estimatedNumElements = InputRef(1).GetMBLayout()->GetNumTimeSteps() * InputRef(1).GetSampleLayout().GetNumElements();
        RequestMatrixFromPool(m_unpackedOperandData, matrixPool, estimatedNumElements, true);
        RequestMatrixFromPool(m_transposedUnpackedOperandData, matrixPool, estimatedNumElements, true);
        RequestMatrixFromPool(m_unpackedConvResultData, matrixPool, estimatedNumElements, true); 
        RequestMatrixFromPool(m_transposedUnpackedConvResultData, matrixPool, estimatedNumElements, true);
        RequestMatrixFromPool(m_tempUnpackedData, matrixPool, estimatedNumElements, true);
        RequestMatrixFromPool(m_transposedUnpackedGrad, matrixPool, estimatedNumElements, true);
        RequestMatrixFromPool(m_unpackedOperandGrad, matrixPool, estimatedNumElements, true);
        RequestMatrixFromPool(m_transposedUnpackedOperandGrad, matrixPool, estimatedNumElements, true);
        RequestMatrixFromPool(m_tempPackedGradientData, matrixPool, estimatedNumElements, true);
    }

    void ReleaseMatricesAfterForwardProp(MatrixPool& matrixPool) override
    {
        Base::ReleaseMatricesAfterForwardProp(matrixPool);
    }

    void RequestMatricesBeforeBackprop(MatrixPool& matrixPool) override
    {
        Base::RequestMatricesBeforeBackprop(matrixPool);
    }

    void ReleaseMatricesAfterBackprop(MatrixPool& matrixPool) override
    {
        Base::ReleaseMatricesAfterBackprop(matrixPool);
        ReleaseMatrixToPool(m_tempGatherIndices, matrixPool);
        ReleaseMatrixToPool(m_tempScatterIndices, matrixPool);
        ReleaseMatrixToPool(m_unpackedOperandData, matrixPool);
        ReleaseMatrixToPool(m_transposedUnpackedOperandData, matrixPool);
        ReleaseMatrixToPool(m_unpackedConvResultData, matrixPool);
        ReleaseMatrixToPool(m_transposedUnpackedConvResultData, matrixPool);
        ReleaseMatrixToPool(m_tempUnpackedData, matrixPool);
        ReleaseMatrixToPool(m_transposedUnpackedGrad, matrixPool);
        ReleaseMatrixToPool(m_unpackedOperandGrad, matrixPool);
        ReleaseMatrixToPool(m_transposedUnpackedOperandGrad, matrixPool);
        ReleaseMatrixToPool(m_tempPackedGradientData, matrixPool);
    }

protected:
    virtual void /*ConvolutionNode::*/InferMBLayoutFromInputs(bool isFinalValidationPass) override
    {
        if (!m_pMBLayout)
            m_pMBLayout = std::make_shared<MBLayout>();
    }

private:

    void TransposeSequenceChannelAxisWithTransposedDims(const shared_ptr<Matrix<ElemType>>& untransposedData, const shared_ptr<Matrix<ElemType>>& toTransposedData,
        const SmallVector<size_t>& transposedDims, const MBLayoutPtr& pMBLayout)
    {
        TensorView<ElemType> untransposedView(untransposedData, transposedDims);
        TransposeSequenceChannelAxisWithTransposedDims(untransposedView, toTransposedData, transposedDims, pMBLayout);
    }

    void TransposeSequenceChannelAxisWithUntransposedDims(const shared_ptr<Matrix<ElemType>>& untransposedData, const shared_ptr<Matrix<ElemType>>& toTransposedData,
        const SmallVector<size_t>& untransposedDims, const MBLayoutPtr& pMBLayout)
    {
        const size_t inputRank = GetInputSampleLayout(OperandInputIdx).GetRank();
        SmallVector<size_t> transposedDims(untransposedDims);
        std::swap(transposedDims[inputRank], transposedDims[inputRank - 1]);

        TransposeSequenceChannelAxisWithTransposedDims(untransposedData, toTransposedData, transposedDims, pMBLayout);
    }

    void TransposeSequenceChannelAxisWithTransposedDims(TensorView<ElemType>& untransposedView, const shared_ptr<Matrix<ElemType>>& toTransposedData,
        const SmallVector<size_t>& transposedDims, const MBLayoutPtr& pMBLayout)
    {
        const size_t inputRank = GetInputSampleLayout(OperandInputIdx).GetRank();
        SmallVector<size_t> untransposedDims(transposedDims);
        std::swap(untransposedDims[inputRank], untransposedDims[inputRank - 1]);
        TransposeSequenceChannelAxisWithUntransposedDims(untransposedView, toTransposedData, untransposedDims, pMBLayout);
    }

    void TransposeSequenceChannelAxisWithUntransposedDims(TensorView<ElemType>& untransposedView, const shared_ptr<Matrix<ElemType>>& toTransposedData,
        const SmallVector<size_t>& untransposedDims, const MBLayoutPtr& pMBLayout)
    {
        TensorShape transposedShape(untransposedDims);
        size_t rank = transposedShape.GetRank();
        UpdateFunctionValuesSize(*toTransposedData, transposedShape, pMBLayout);

        transposedShape.AppendInPlace(rank++, pMBLayout->GetNumParallelSequences());
        transposedShape.AppendInPlace(rank++, pMBLayout->GetNumTimeSteps());
        const size_t inputRank = GetInputSampleLayout(OperandInputIdx).GetRank();
        transposedShape.SwapDimsInPlace(inputRank - 1, inputRank);

        untransposedView = untransposedView.Reshaped(transposedShape);

        TensorView<ElemType> transposedView(toTransposedData, transposedShape.GetDims());
        transposedView.AssignCopyOf(untransposedView);
    }

    void ToSequenceGradient()
    {
        auto numSequences = GetMBLayout()->GetNumSequences();
        auto gradientSampleLayout = m_unpackedOperandShape;
        auto gradientDataTensorShape = gradientSampleLayout;
        gradientDataTensorShape.AppendInPlace(gradientDataTensorShape.GetRank(), numSequences);
        let& gradientDataMatrix = *m_transposedUnpackedOperandGrad;
        auto gradientDataNDArrayView = ::CNTK::MakeSharedObject<::CNTK::NDArrayView>(::CNTK::AsDataType<ElemType>(),
            ::CNTK::AsDeviceDescriptor(gradientDataMatrix.GetDeviceId()),
            ::CNTK::AsStorageFormat(gradientDataMatrix.GetFormat()),
            ::CNTK::AsNDShape(gradientDataTensorShape),
            /*readOnly =*/true,
            new TensorView<ElemType>(m_transposedUnpackedOperandGrad, gradientDataTensorShape));

        auto gradientDataValue = ::CNTK::MakeSharedObject<::CNTK::Value>(gradientDataNDArrayView, ::CNTK::CreateMask(m_inputSequenceLengths));
        auto dummyVar = ::CNTK::InputVariable(::CNTK::AsNDShape(InputRef(1).GetSampleLayout()), gradientDataNDArrayView->IsSparse(), ::CNTK::AsDataType<ElemType>());
        auto packedGradientMatrixAndLayout = ::CNTK::Utils::GetCNTKImplMatrixAndMBLayoutFromValueObject(dummyVar, gradientDataValue, nullptr, m_tempPackedGradientData, m_tempGatherIndices);

        if (*packedGradientMatrixAndLayout.second != *InputRef(1).GetMBLayout())
            LogicError("%ls: %s node unpacked gradient MBLayout does not match input MBLayout.", Base::NodeDescription().c_str(), typeid(*this).name());

        InputRef(1).Gradient() += (*packedGradientMatrixAndLayout.first);
    }

    void UnpackAndTransposeGradient()
    {
        ElemType gapPadValue = 0;
        TensorView<ElemType> unpackedGradientView = ComputationNode<ElemType>::Unpack(GetSampleLayout(), Gradient(), m_pMBLayout, m_tempUnpackedData, m_tempScatterIndices, 
            std::shared_ptr<Matrix<char>>(nullptr), /*batchMajor=*/ false, &gapPadValue);

        // TODO : set transposed unpacked gradient shape. 
        // We have m_unpackedConvResultShape, which is the desired transpose output shape for gradient (with the appending dynamic axes shape).
        // however, we need to obtain the shape for input from method SwapDimsInPlace result, which is crucial for the strides setup.
        // and the SwapDimsInPlace requires us computing the original shape for unpacked gradient first. So we swap back, construct shape, and swap again. 
        TensorShape unpackedGradientShape(m_unpackedConvResultShape);
        const size_t inputRank = GetInputSampleLayout(OperandInputIdx).GetRank();
        unpackedGradientShape.SwapDimsInPlace(inputRank - 1, inputRank);
        TensorShape transposedUnpackedGradientShape(unpackedGradientShape.GetDims());

        size_t unpackedGradientShapeRank = transposedUnpackedGradientShape.GetRank();
        transposedUnpackedGradientShape.AppendInPlace(unpackedGradientShapeRank++, m_unpackedpMBLayout->GetNumParallelSequences());
        transposedUnpackedGradientShape.AppendInPlace(unpackedGradientShapeRank++, m_unpackedpMBLayout->GetNumTimeSteps());
        transposedUnpackedGradientShape.SwapDimsInPlace(inputRank - 1, inputRank);
        unpackedGradientView = unpackedGradientView.Reshaped(transposedUnpackedGradientShape);

        UpdateFunctionValuesSize(*m_transposedUnpackedGrad, unpackedGradientShape, m_unpackedpMBLayout);
        
        m_transposedUnpackedGradientView = TensorView<ElemType>(m_transposedUnpackedGrad, transposedUnpackedGradientShape.GetDims());
        m_transposedUnpackedGradientView.AssignCopyOf(unpackedGradientView);
    }

    void UpdateFunctionValuesSize(Matrix<ElemType>& m, const TensorShape& shape, const MBLayoutPtr& pMBLayout)
    {
        // Same computing logic in ComputationNode.
        // The original method only computes and updates its own output matrix. Here we need for arbitrary ones. 
        size_t rows, cols;

        if (m_isValueSparse && !!pMBLayout)
        {
            size_t rank = shape.GetRank();
            rows = rank > 0 ? shape[0] : 1;

            cols = 1;
            for (size_t k = 1; k < rank; k++)   // all dimensions except leading one
                cols *= shape[k];

            cols *= pMBLayout->GetNumCols();
        }
        else
        {
            if (!!pMBLayout)
            {
                rows = shape.GetNumElements();
                cols = pMBLayout->GetNumCols();
            }
            else
            {
                size_t rank = shape.GetRank();
                rows = rank > 0 ? shape[0] : 1;
                cols = 1;
                for (size_t k = 1; k < rank; k++)   // all dimensions except leading one
                    cols *= shape[k];
            }
        }

        m.Resize(rows, cols, /*keepValue*/false);
        m.CollapseDataLocation();
    }

    std::vector<size_t> GetOutputSequenceLengths(const std::vector<size_t>& inputSequenceLengths)
    {
        // Same computing logic in ConvolveGeometry.
        // Rewritten here because TensorShape uses SmallVector which cannot exceed size of 12, a size too small for sequence count.
        size_t kernelShape_i = m_kernelShape[m_seqAxisIdx];
        size_t delta = m_stride[m_seqAxisIdx];
        size_t dil = m_dilation[m_seqAxisIdx];
        bool autoPadCur = m_autoPad[m_seqAxisIdx];
        size_t lo = m_lowerPad[m_seqAxisIdx];
        size_t hi = m_upperPad[m_seqAxisIdx];
        size_t effectiveKernelShape = (kernelShape_i - 1) * dil + 1;

        size_t dimAdjust = 0;

        if (autoPadCur)
        {
            dimAdjust += dil * (kernelShape_i - 1);
        }
        else
        {
            dimAdjust += lo + hi;
        }

        dimAdjust -= effectiveKernelShape;

        std::vector<size_t> outputSequenceLengths(inputSequenceLengths);
        for (size_t i = 0; i < outputSequenceLengths.size(); ++i)
        {
            float preciseDimOut = (float)(outputSequenceLengths[i] + dimAdjust) / delta + 1;
            // TODO: ceilOutDim?
            outputSequenceLengths[i] = static_cast<size_t>(floor(preciseDimOut));
        }

        return outputSequenceLengths;
    }

    TensorShape EmulateInputShape(bool isFinalValidationPass, size_t& emulatedAxisIdx, bool transposed = true)
    {
        const size_t inputIdx = GetExpectedNumInputs() - 1;
        TensorShape inputShape = GetInputSampleLayout(inputIdx);
        TensorShape outputShape;
        TensorShape emulatedInputShape = inputShape;
        const size_t inputRank = inputShape.GetRank();
        if (!isFinalValidationPass)
            emulatedInputShape.AppendInPlace(inputRank, ::CNTK::NDShape::FreeDimension);
        else
        {
            auto inputMBLayout = Input(inputIdx)->GetMBLayout();
            if (!inputMBLayout)
                InvalidArgument("%ls %ls operation can only operate on minibatch data (which have a layout).", NodeName().c_str(), OperationName().c_str());
            if (inputMBLayout->GetNumTimeSteps() == 0)
                LogicError("%ls %ls operation's final validation pass must not be invoked before the input MBLayout has been initialized and populated.", 
                    NodeName().c_str(), OperationName().c_str());

            emulatedInputShape.AppendInPlace(inputRank, inputMBLayout->GetNumTimeSteps());
        }
        if (transposed)
            emulatedInputShape.SwapDimsInPlace(inputRank - 1, inputRank);
        // BUGGY here. Swap seems wrong as m_strides is incorrect. At least for this case. 
        // Update: it seems the m_strides unmatching is intended. The problem seems we must append dynamic axes first and then do swaps. 
        //SmallVector<size_t> dims = emulatedInputShape.GetDims();
        //std::swap(dims[inputRank - 1], dims[inputRank]);
        //emulatedInputShape = TensorShape(dims);
        
        emulatedAxisIdx = inputRank - 1;
        return emulatedInputShape;
    }

    TensorShape EmulateInputShape(bool isFinalValidationPass, bool transposed = true)
    {
        size_t unusedEmulatedAxisIdx = 0;
        return EmulateInputShape(isFinalValidationPass, unusedEmulatedAxisIdx, transposed);
    }

    TensorShape GetTransposedShapeFromEmulatedOutputShape(const TensorShape& outputShape, size_t emulatedAxisIdx)
    {
        if (emulatedAxisIdx + 1 >= outputShape.GetRank())
            RuntimeError("%ls %ls operation shouldn't have transpose index (%d) >= rank (%d). ",
                NodeName().c_str(), OperationName().c_str(), emulatedAxisIdx + 1, outputShape.GetRank());
        //TensorShape transposedOutputShape = outputShape;
        //transposedOutputShape.SwapDimsInPlace(emulatedAxisIdx, emulatedAxisIdx + 1);
        SmallVector<size_t> dims = outputShape.GetDims();
        std::swap(dims[emulatedAxisIdx], dims[emulatedAxisIdx + 1]);
        TensorShape transposedOutputShape(dims);

        return transposedOutputShape;
    }

    TensorShape GetOutputShapeFromEmulatedOutputShape(const TensorShape& outputShape, size_t emulatedAxisIdx)
    {
        if (outputShape.GetRank() <= emulatedAxisIdx)
            LogicError("%ls %ls operation's emulated output shape should have rank(%d) >= emulated static axis index(%d).",
                NodeName().c_str(), OperationName().c_str(), outputShape.GetRank(), emulatedAxisIdx);
        SmallVector<size_t> dims;
        for (size_t i = 0; i < outputShape.GetRank(); i++)
        {
            if (i != emulatedAxisIdx)
                dims.push_back(outputShape[i]);
        }
        // TODO : here might have bug as I'm not sure if strides are set correctly for this shape. 
        return dims;
    }

    void ValidateUnpack(bool isFinalValidationPass)
    {
        // Prepare MBLayout and Shape for intermediate unpack result. 
        if (!m_unpackedpMBLayout)
        {
            m_unpackedpMBLayout = std::make_shared<MBLayout>();
            m_unpackedpMBLayout->SetUniqueAxisName(Base::DefaultNoSequenceAxisName);
        }
        m_unpackedOperandShape = EmulateInputShape(isFinalValidationPass, false);
    }

    void ValidateConvolution(bool isFinalValidationPass)
    {
        // Need to insert seq axis into input shape. Run ConvolutionNode::Validate with emulated inputOperandShape. 
        const size_t inputIdx = GetExpectedNumInputs() - 1;
        size_t emulatedAxisIdx = 0;
        TensorShape inputShape = GetInputSampleLayout(inputIdx);
        TensorShape emulatedInputShape = EmulateInputShape(isFinalValidationPass, emulatedAxisIdx);

        Input(inputIdx)->SetDims(emulatedInputShape, Input(inputIdx)->HasMBLayout());
        // ConvolutionNode::Validate checks computed output shape against user defined output shape. 
        // We cannot emulate static sequence axis for them because in different stages it might be NDShape::FreeDimension or size_t.
        // We move the check out of Base::Validate by setting m_outputShape to rank 0 now, and check here later. 
        TensorShape tempOutputShape;
        bool checkUserDefinedOutputShape = (m_outputShape.GetRank() > 0 && m_outputShape != TensorShape(0));
        if (checkUserDefinedOutputShape)
        {
            tempOutputShape = m_outputShape;
            m_outputShape = TensorShape();
        }

        // TODO : since output mblayout differs from input, we need to create new mblayout. This is different from Base. 
        Base::Validate(isFinalValidationPass);
        Input(inputIdx)->SetDims(inputShape, Input(inputIdx)->HasMBLayout());

        // Update member with computed emulated output shape. 
        m_unpackedConvResultShape = GetSampleLayout();
        m_transposedUnpackedConvResultShape = GetTransposedShapeFromEmulatedOutputShape(m_unpackedConvResultShape, emulatedAxisIdx);
        // The outputShape contains the emulated static axis for sequence axis. We need to remove that as now we convert back to sequence. 
        TensorShape outputShape = GetOutputShapeFromEmulatedOutputShape(m_unpackedConvResultShape, emulatedAxisIdx);
        Base::SetDims(outputShape, HasMBLayout());

        if (checkUserDefinedOutputShape)
        {
            m_outputShape = tempOutputShape;
            if (m_outputShape != outputShape)
            {
                InvalidArgument("%ls %ls the shape of the specified convolution output %ls is different from "
                    "the result of convoluting the input argument using the provided options %ls. It is recommended "
                    "that the output shape is not specified for convolution.", NodeName().c_str(), OperationName().c_str(),
                    static_cast<std::wstring>(m_outputShape).c_str(),
                    static_cast<std::wstring>(outputShape).c_str());
            }
        }
    }

    void ValidateToSequence(bool isFinalValidationPass)
    {
        const size_t inputIdx = GetExpectedNumInputs() - 1;
        ComputationNodeBase::m_isValueSparse = Input(inputIdx)->IsValueSparse();

        if (!m_pMBLayout)
        {
            m_pMBLayout = std::make_shared<MBLayout>();
        }
    }

private:
    size_t m_seqAxisIdx;
    bool m_isGradientUnpackedAndTransposed;

    std::vector<size_t> m_inputSequenceLengths;

    TensorShape m_unpackedOperandShape;
    TensorShape m_unpackedConvResultShape;
    TensorShape m_transposedUnpackedConvResultShape;
    MBLayoutPtr m_unpackedpMBLayout;
    shared_ptr<Matrix<ElemType>> m_unpackedOperandData;
    shared_ptr<Matrix<ElemType>> m_transposedUnpackedOperandData;
    shared_ptr<Matrix<ElemType>> m_unpackedConvResultData;
    shared_ptr<Matrix<ElemType>> m_transposedUnpackedConvResultData;

    TensorView<ElemType> m_transposedUnpackedGradientView;
    shared_ptr<Matrix<ElemType>> m_transposedUnpackedGrad;
    shared_ptr<Matrix<ElemType>> m_unpackedOperandGrad;
    shared_ptr<Matrix<ElemType>> m_transposedUnpackedOperandGrad;
    //shared_ptr<Matrix<ElemType>> 

    // shared by unpack and tosequence
    shared_ptr<Matrix<ElemType>> m_tempGatherIndices;
    shared_ptr<Matrix<ElemType>> m_tempScatterIndices;

    // unpack
    shared_ptr<Matrix<ElemType>> m_tempUnpackedData;
    // tosequence
    shared_ptr<Matrix<char>> m_tempMask;
    shared_ptr<Matrix<ElemType>> m_tempPackedGradientData;
};



// -----------------------------------------------------------------------
// ROIPoolingNode (inputFeatures, inputROIs)--pooling for object detection.
//
// Each input image has a fixed number of regions of interest (ROIs),
// specified as bounding boxes (x, y, w, h) that are relative to the
// image size [W x H]. This node is meant as a replacement for the
// final pooling layer of an image classification network.  The first
// fully-connected layer expects a fixed size input, but for object
// detection we want each ROI to look like an image to the network so
// we can get a label for it. The ROIs have different spatial sizes,
// so this node does Max Pooling, but with an adaptive pooling window,
// so that each ROI output has the spatial size expected by the first
// fully-connected layer. Images are Input(0). ROIs are Input(1).
//
// Input0: Images       [W x H x C x N]
// Input1: ROIs         [4 x roisPerImage x N],
// output: Pooled ROIs  [PW x PH x C x roisPerImage x N]
// where PW = Pooled Width, PH = Pooled Height, C = Channels, N = Batch Size
//
// See http://arxiv.org/abs/1504.08083
// -----------------------------------------------------------------------
template <class ElemType>
class ROIPoolingNode : public ComputationNode<ElemType>, public NumInputs<2>
{
    typedef ComputationNode<ElemType> Base; UsingComputationNodeMembersBoilerplate;
    static const std::wstring TypeName() { return L"ROIPooling"; }

public:
    ROIPoolingNode(DEVICEID_TYPE deviceId, const wstring& name, PoolKind poolKind = PoolKind::Max, const TensorShape& roiOutputShape = TensorShape(), double spatialScale = 1.0/16.0)
        : Base(deviceId, name), m_poolKind(poolKind), m_roiOutputShape(roiOutputShape), m_spatialScale(spatialScale), m_argmaxData(Matrix<ElemType>::Zeros(1, 1, deviceId))
    {
    }

    ROIPoolingNode(const ScriptableObjects::IConfigRecordPtr configp)
        : ROIPoolingNode(configp->Get(L"deviceId"), L"<placeholder>", PoolKindFrom(configp->Get(L"pool")), configp->Get(L"roiOutputShape"), configp->Get(L"featureScale"))
    {
        AttachInputsFromConfig(configp, GetExpectedNumInputs());
    }

    void RequestMatricesBeforeForwardProp(MatrixPool& matrixPool) override
    {
        Base::RequestMatricesBeforeForwardProp(matrixPool);
        size_t matrixSize = m_sampleLayout.GetNumElements();
        RequestMatrixFromPool(m_tempMatrix, matrixPool, matrixSize, true);
    }

    // m_tempMatrix cannot be released after Forward Prop because its content (argmax) is used for back prop.

    void ReleaseMatricesAfterBackprop(MatrixPool& matrixPool) override
    {
        Base::ReleaseMatricesAfterBackprop(matrixPool);
        ReleaseMatrixToPool(m_tempMatrix, matrixPool);
    }

    // Input0: Images       [W x H x C x N]
    // Input1: ROIs         [4 x roisPerImage x N],
    // output: Pooled ROIs  [PW x PH x C x roisPerImage x N]
    // where PW = Pooled Width, PH = Pooled Height, C = Channels, N = Batch Size
    //
    // Explanation: this node has a target output shape of
    // [Pooled Width x Pooled Height x Channels], as does any pooling
    // layer. However, we want each /ROI/ to have that output size,
    // not each image. After this node, operations in the network
    // should be on ROIs, not on the full images. The forward pass
    // loops over images and the ROIs associated with each image; for
    // every ROI, it treats the subset of the image specified by that
    // ROI as a full image and does max pooling over that subset,
    // using whatever window size will correspond to an output of
    // [Pooled Width x Pooled Height x Channels]. Hence,
    // the output tensor is [PW x PH x C x roisPerImage x N]
    // An example validation output looks like this:
    // Validating --> z.roiOut = ROIPooling (z.conv5Out.conv5.y, rois) : [61 x 61 x 256 x *], [4 x 64 x *] -> [6 x 6 x 256 x 64 x *]
    void ForwardProp(const FrameRange& fr) override
    {
        // [4 x roisPerImage x N] -- first dimension is roiSize (4), second is rois-per-image, third is mb size
        size_t roisPerImage = (size_t)GetInputSampleLayout(1)[1];

        auto inputShape = GetInputSampleLayout(0);
        Matrix<ElemType> inputSlice = Input(0)->ValueFor(fr);
        Matrix<ElemType> ROIs = Input(1)->ValueFor(fr);

        // our output slice for this minibatch.
        Matrix<ElemType> outputSlice = ValueFor(fr);

        // input slice is [W x H x C x N]; cols are images.
        // ROIs is [4 x roisPerImage x N]; cols are ROIs for different images.
        // each ROI is (x, y, w, h) relative to original image size.
        size_t inputW = (size_t)inputShape[0];
        size_t inputH = (size_t)inputShape[1];
        size_t numChannels = (size_t)inputShape[2];
        size_t outW = m_roiOutputShape[0];
        size_t outH = m_roiOutputShape[1];

        m_tempMatrix->Resize(outW * outH * numChannels * roisPerImage, inputSlice.GetNumCols());
        if (m_poolKind == PoolKind::Max)
            inputSlice.MaxROIPoolingForward(roisPerImage, inputSlice.GetNumCols(),
                numChannels, inputW, inputH, outW, outH, ROIs, outputSlice, *m_tempMatrix, m_spatialScale);
        else
            LogicError("Average ROI pooling is not supported.");
    }

    // similar to usual MaxPooling backpropagation. Send gradients
    // back through to the locations that were used as the "max." Only
    // difference: needs to sum gradients over all the ROIs that may
    // have used that location. One image location could be in
    // multiple ROIs--in that case each ROI may contribute a gradient term.
    void BackpropTo(const size_t inputIndex, const FrameRange& fr) override
    {
        if (inputIndex != 0)
            return;

        auto inputShape = GetInputSampleLayout(0);
        Matrix<ElemType> inputSlice = Input(0)->ValueFor(fr);

        int inputW = inputShape[0];
        int inputH = inputShape[1];
        int numChannels = inputShape[2];

        auto inputGrad = Input(0)->GradientFor(fr);
        auto pooledGrad = GradientFor(fr);

        int roisPerImage = GetInputSampleLayout(1)[1];
        auto roiData = Input(1)->ValueFor(fr);

        if (m_poolKind == PoolKind::Max)
            pooledGrad.MaxROIPoolingBackward(roisPerImage, inputSlice.GetNumCols(), numChannels,
                inputW, inputH, m_roiOutputShape[0], m_roiOutputShape[1], roiData, inputGrad, *m_tempMatrix, m_spatialScale);
        else
            LogicError("Average ROI pooling is not supported.");
    }

    void Save(File& fstream) const override
    {
        Base::Save(fstream);
        m_roiOutputShape.Save(fstream);
        fstream << (int32_t)m_poolKind;
        fstream << m_spatialScale;
    }

    void Load(File& fstream, size_t modelVersion) override
    {
        Base::Load(fstream, modelVersion);
        m_roiOutputShape.Load(fstream);

        if (modelVersion < CNTK_MODEL_VERSION_26)
        {
            // There are 2 problems here:
            //    1. m_spatialScale value depends on your location in the network, for current R-CNN and its family it is 1/16.
            //    2. roiData format also has changed from ratio to absolute values and those are given as input.
            m_poolKind = PoolKind::Max;
            m_spatialScale = 1.0/16.0;
        }
        else
        {
            int32_t k;
            fstream >> k;
            m_poolKind = (PoolKind)k;
            fstream >> m_spatialScale;
        }
    }

    void Validate(bool isFinalValidationPass) override
    {
        Base::Validate(isFinalValidationPass);
        InferMBLayoutFromInputsForStandardCase(isFinalValidationPass);

        auto inShape = GetInputSampleLayout(0);   // layout of input shape is width x height x numChannels
        auto roiShape = GetInputSampleLayout(1); // layout of ROI shape is 4 x roisPerImage

        if (isFinalValidationPass && (m_roiOutputShape.size() != 2))
            InvalidArgument("ROIPoolingNode: roi output shape must have two dimensions ([W x H]).");

        if (isFinalValidationPass && (inShape[0] < m_roiOutputShape[0] || inShape[1] < m_roiOutputShape[1]))
            InvalidArgument("ROIPoolingNode: inputWidth must >= windowWidth and inputHeight must >= windowHeight.");

        if (isFinalValidationPass && (inShape[2] < 1))
            InvalidArgument("ROIPoolingNode: input must have at least one channel ([W x H x C]).");

        if (isFinalValidationPass && (roiShape[0] != 4))
            InvalidArgument("ROIPoolingNode: ROI input must have the following shape: [4 x roisPerImage].");

        if (isFinalValidationPass && (roiShape[1] < 1))
            InvalidArgument("ROIPoolingNode: ROI input must contain at least one ROI ([4 x roisPerImage]).");

        // set output dimensions to [W x H x C x roisPerImage]
        SetDims(TensorShape(m_roiOutputShape[0], m_roiOutputShape[1], inShape[2], roiShape[1]), HasMBLayout());
    }

    void CopyTo(ComputationNodeBasePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const override
    {
        Base::CopyTo(nodeP, newName, flags);
        if (flags & CopyNodeFlags::copyNodeValue)
        {
            auto node = dynamic_pointer_cast<ROIPoolingNode<ElemType>>(nodeP);
            node->m_poolKind = m_poolKind;
            node->m_roiOutputShape = m_roiOutputShape;
            node->m_spatialScale = m_spatialScale;
        }
    }

    PoolKind PoolingKind() const { return m_poolKind; }
    TensorShape ROIOutputShape() const { return m_roiOutputShape; }
    double SpatialScale() const { return m_spatialScale; }

protected:
    PoolKind m_poolKind;
    TensorShape m_roiOutputShape;
    double m_spatialScale;
    shared_ptr<Matrix<ElemType>> m_tempMatrix;
    Matrix<ElemType> m_argmaxData;
};

// -----------------------------------------------------------------------
// PoolingNode (inputFeature)
// Performs max or average ND pooling.
// -----------------------------------------------------------------------

template <class ElemType>
class PoolingNode : public ConvolutionNodeBase<ElemType>, public NumInputs<1>, public TransformerNode
{
    typedef ConvolutionNodeBase<ElemType> Base; UsingConvolutionNodeBaseMembers;
    static const std::wstring TypeName() { return L"Pooling"; }
public:
    PoolingNode(DEVICEID_TYPE deviceId, const wstring& name)
        : Base(deviceId, name)
    {
    }
    PoolingNode(DEVICEID_TYPE deviceId, const wstring& name, PoolKind pool, const TensorShape& kernelShape, const TensorShape& strideShape,
                const std::vector<bool>& autoPadding, const TensorShape& lowerPad, const TensorShape& upperPad, bool ceilOutDim, const bool poolIncludePad,
                ImageLayoutKind imageLayout)
                : Base(deviceId, name, kernelShape, TensorShape(1), strideShape, vector<bool>{true}, autoPadding, lowerPad, upperPad, pool, poolIncludePad, false, TensorShape(0), ceilOutDim, imageLayout, 0)
    {
    }
    PoolingNode(const ScriptableObjects::IConfigRecordPtr configp)
        : PoolingNode(configp->Get(L"deviceId"), L"<placeholder>", PoolKindFrom(configp->Get(L"pool")), configp->Get(L"kernelShape"),
                      configp->Get(L"strideShape"),
                      configp->Get(L"dimPadding"), configp->Get(L"dimPadLower"), configp->Get(L"dimPadUpper"), configp->Get(L"ceilOut"), configp->Get(L"poolIncludePad"),
                      ImageLayoutKindFrom(configp->Get(L"imageLayout")))
    {
        AttachInputsFromConfig(configp, GetExpectedNumInputs());
    }

public:
    void ForwardProp(const FrameRange& fr) override
    {
        Matrix<ElemType> sliceOutputValue = ValueFor(fr);
        const Matrix<ElemType>& input0 = InputRef(0).ValueFor(fr);
        m_convEng->ForwardPooling(input0, sliceOutputValue);
    }

    void BackpropTo(const size_t inputIndex, const FrameRange& fr) override
    {
        auto sliceOutputGrad = GradientFor(fr);
        Matrix<ElemType> sliceInput0Grad = InputRef(0).GradientFor(fr);
        Matrix<ElemType> sliceInput0Value = InputRef(0).ValueFor(fr);
        Matrix<ElemType> sliceOutputValue = ValueFor(fr);

        m_convEng->BackwardPooling(sliceOutputValue, sliceOutputGrad, sliceInput0Value, sliceInput0Grad, !InputRef(0).IsGradientInitializedBy(this));
    }

    bool OutputUsedInComputingInputNodesGradients() const override
    {
        // The PoolingNode requires output values only for max pooling.
        return m_poolKind == PoolKind::Max;
    }

    virtual ParentGradientOptimization ImplementsGradientOptimization(const ComputationNodeBase*) const override
    {
        return ParentGradientOptimization::Overwrite;
    }

public:
    void Validate(bool isFinalValidationPass) override
    {
        Base::Validate(isFinalValidationPass);
        InferMBLayoutFromInputsForStandardCase(isFinalValidationPass);

        if (m_imageLayout != ImageLayoutKind::CHW)
        {
            InvalidArgument(
                "%ls %ls supports only cuDNN (CHW) data layout. "
                "Please specify imageLayout=\"cudnn\" in %ls node in your script "
                "and make sure input data layout is CHW", NodeName().c_str(), OperationName().c_str(), NodeName().c_str());
        }

        const auto& inputShape = GetInputSampleLayout(0);

        // infer reduction dimensions if not given
        InferReductionDims(inputShape, TensorShape());

        auto outDims = this->ComputeOutputShape(inputShape, TensorShape(1), m_ceilOutDim, isFinalValidationPass);
        SetDims(outDims, HasMBLayout());
        if (isFinalValidationPass)
        {
            bool recomputeConvGeometry = (m_convEng == nullptr) ? false : // For first minibatch, this flag must be false, so initial mem allocation can happen.
                (outDims != m_convEng->Geometry()->OutputShape()) || (inputShape != m_convEng->Geometry()->InputShape());
            if ((m_convEng == nullptr) || ((m_convEng != nullptr) && recomputeConvGeometry))
            {
                auto geometry = std::make_shared<ConvolveGeometry>(inputShape, m_kernelShape, m_mapCount, m_stride,
                                                                   m_sharing, m_autoPad, m_lowerPad, m_upperPad, TensorShape(1), m_ceilOutDim);
                m_convEng = ConvolutionEngine<ElemType>::Create(geometry, m_deviceId, m_imageLayout,
                                                                m_maxTempMemSizeInSamples, m_poolKind,
                                                                ConvolutionEngineKind::All, NodeName(), Globals::ShouldForceDeterministicAlgorithms(),
                                                                m_poolIncludePad, recomputeConvGeometry);
            }
        }
    }

private:
    using TransformerNode::m_transforms;
    using ConvolutionNodeBase<ElemType>::ComputeFilterTransform;

    virtual void /*TransformerNode::*/ComputeTransforms() override
    {
        if (m_transforms[0].m_axisTransforms.empty())
        {
            m_transforms[0] = ComputeFilterTransform();
            m_transforms[0] = m_transforms[0].Inverse();
        }
        // else: transform already computed, no need to do it again.
    }

    virtual bool /*TransformerNode::*/SupportsTransformOnInput(size_t /*inputIndex*/) override
    {
        // We support transforms on all inputs (one here).
        return true;
    }
};

// -----------------------------------------------------------------------
// MaxUnpoolingNode (unpoolInputValues, poolInputValues)
// Performs "max unpooling" operation. Max unpooling mirrors the operation
// performed by max pooling node and depends on the values provided to
// the max pooling node (so unlike deconvolution operation, it is not
// completely independent). Unpooling takes 2 inputs: features to be unpooled,
// which tensor has the same shape as corresponding max pooling node output
// and inputs for the original pooling node. Unpooling node
// produces an output which has the same dimensions as input to the
// corresponding max pooling node (i.e. poolInputValues).
// TODO: need to add support for other pooling types, for example,
// average unpooling. Note that in this case, generic unpooling operation
// will take different number of inputs depending on pooling type.
// -----------------------------------------------------------------------

template <class ElemType>
class MaxUnpoolingNode : public ConvolutionNodeBase<ElemType>, public NumInputs<2>, public TransformerNode
{
    typedef ConvolutionNodeBase<ElemType> Base;
    UsingConvolutionNodeBaseMembers;
    static const std::wstring TypeName() { return L"MaxUnpooling"; }

public:
    MaxUnpoolingNode(DEVICEID_TYPE deviceId, const wstring& name)
        : Base(deviceId, name)
    {
    }
    MaxUnpoolingNode(DEVICEID_TYPE deviceId, const wstring& name, const TensorShape& kernelShape, const TensorShape& strideShape,
                       const std::vector<bool>& autoPadding, const TensorShape& lowerPad, const TensorShape& upperPad,
                       ImageLayoutKind imageLayout)
                       : Base(deviceId, name, kernelShape, TensorShape(1), strideShape, vector<bool>{true}, autoPadding, lowerPad, upperPad, PoolKind::Max, false, true, TensorShape(0), false, imageLayout, 0)
    {
    }
    MaxUnpoolingNode(const ScriptableObjects::IConfigRecordPtr configp)
        : MaxUnpoolingNode(configp->Get(L"deviceId"), L"<placeholder>", configp->Get(L"kernelShape"),
                           configp->Get(L"strideShape"), configp->Get(L"dimPadding"), configp->Get(L"dimPadLower"), configp->Get(L"dimPadUpper"),
                           ImageLayoutKindFrom(configp->Get(L"imageLayout")))
    {
        AttachInputsFromConfig(configp, GetExpectedNumInputs());
    }

public:
    void ForwardProp(const FrameRange& fr) override
    {
        const Matrix<ElemType>& unpoolInput = InputRef(0).ValueFor(fr);
        const Matrix<ElemType>& poolInput = InputRef(1).ValueFor(fr);
        Matrix<ElemType> sliceOutputValue = ValueFor(fr);
        m_convEng->MaxUnpooling(unpoolInput, poolInput, sliceOutputValue);
    }

    void BackpropTo(const size_t inputIndex, const FrameRange& fr) override
    {
        if (inputIndex != 0)
            return;

        auto sliceOutputGrad = GradientFor(fr);
        Matrix<ElemType> sliceInput0Grad = InputRef(0).GradientFor(fr);
        // BUGBUG: ForwardPooling overwrites values in sliceInput1Grad. Should handle correctly instead.
        m_convEng->ForwardPooling(sliceOutputGrad, sliceInput0Grad);
    }

    bool OutputUsedInComputingInputNodesGradients() const override { return false; }

    void Validate(bool isFinalValidationPass) override
    {
        Base::Validate(isFinalValidationPass);
        InferMBLayoutFromInputsForStandardCase(isFinalValidationPass);

        if (m_imageLayout != ImageLayoutKind::CHW)
        {
            InvalidArgument(
                "%ls %ls supports only cuDNN (CHW) data layout. "
                "Please specify imageLayout=\"cudnn\" in %ls node in your script "
                "and make sure input data layout is CHW", NodeName().c_str(), OperationName().c_str(), NodeName().c_str());
        }

        auto inputShape = GetInputSampleLayout(0);

        // infer reduction dimensions if not given
        InferReductionDims(inputShape, TensorShape());

        // Same as in case of deconvolution, node input (inputShape) is really the output of the max pooling
        // and node output (outDims) is pooling input.
        auto outputShape = GetInputSampleLayout(1);
        auto inferredShape = this->ComputeOutputShape(outputShape, TensorShape(1), false, isFinalValidationPass);
        if (inputShape != inferredShape)
            InvalidArgument("%ls %ls the shape of the unpooling operand %ls is different from "
                            "the result of pooling the poolingInput argument using"
                            "the provided options %ls", NodeName().c_str(), OperationName().c_str(),
                            static_cast<std::wstring>(inputShape).c_str(),
                            static_cast<std::wstring>(inferredShape).c_str());

        SetDims(outputShape, HasMBLayout());
        if (isFinalValidationPass)
        {
            bool recomputeConvGeometry = (m_convEng == nullptr) ? false : // For first minibatch, this flag must be false, so initial mem allocation can happen.
                (outputShape != m_convEng->Geometry()->OutputShape()) || (inputShape != m_convEng->Geometry()->InputShape());
            if ((m_convEng == nullptr) || ((m_convEng != nullptr) && recomputeConvGeometry))
            {
                auto geometry = std::make_shared<ConvolveGeometry>(outputShape, m_kernelShape, m_mapCount, m_stride,
                                                                   m_sharing, m_autoPad, m_lowerPad, m_upperPad);
                // Create reference engine as it's the only engine that implements unpooling.
                m_convEng = ConvolutionEngine<ElemType>::Create(geometry, m_deviceId, m_imageLayout,
                                                                m_maxTempMemSizeInSamples, m_poolKind,
                                                                ConvolutionEngineKind::Reference,
                                                                NodeName(), false, false, recomputeConvGeometry);
            }
        }
    }

private:
    using TransformerNode::m_transforms;
    using ConvolutionNodeBase<ElemType>::ComputeFilterTransform;

    virtual void /*TransformerNode::*/ComputeTransforms() override
    {
        if (m_transforms.empty())
        {
            m_transforms[0] = ComputeFilterTransform();
        }
        // else: transform already computed, no need to do it again.
    }

    virtual bool /*TransformerNode::*/SupportsTransformOnInput(size_t inputIndex) override
    {
        // We support transform for just unpool input.
        return (inputIndex == 0);
    }
};

// -----------------------------------------------------------------------
// Legacy PoolingNodeBase (input)
// -----------------------------------------------------------------------

template <class ElemType>
class PoolingNodeBase : public ComputationNode<ElemType>, public NumInputs<1>
{
    typedef ComputationNode<ElemType> Base;
    UsingComputationNodeMembers;

public:
    PoolingNodeBase(DEVICEID_TYPE deviceId, const wstring& name, PoolKind poolKind)
        : Base(deviceId, name),
        m_windowWidth(SIZE_MAX),
        m_windowHeight(SIZE_MAX),
        m_horizontalSubsample(SIZE_MAX),
        m_verticalSubsample(SIZE_MAX),
        m_imageLayoutKind(ImageLayoutKind::HWC),
        m_poolKind(poolKind)
    {
    }
    PoolingNodeBase(DEVICEID_TYPE deviceId, const wstring& name, const size_t windowWidth, const size_t windowHeight, const size_t horizontalSubsample, const size_t verticalSubsample, ImageLayoutKind imageLayoutKind, PoolKind poolKind)
        : Base(deviceId, name),
        m_windowWidth(windowWidth),
        m_windowHeight(windowHeight),
        m_horizontalSubsample(horizontalSubsample),
        m_verticalSubsample(verticalSubsample),
        m_imageLayoutKind(imageLayoutKind),
        m_poolKind(poolKind)
    {
        ConvertToTensorShape();
    }
    PoolingNodeBase(const ScriptableObjects::IConfigRecordPtr configp, PoolKind poolKind)
        : PoolingNodeBase(configp->Get(L"deviceId"),
            L"<placeholder>",
            configp->Get(L"windowWidth"),
            configp->Get(L"windowHeight"),
            configp->Get(L"horizontalSubsample"),
            configp->Get(L"verticalSubsample"),
            ImageLayoutKindFrom(configp->Get(L"imageLayout")),
            poolKind)
    {
        // input, windowWidth, windowHeight, horizontalSubsample, verticalSubsample
        AttachInputsFromConfig(configp, this->GetExpectedNumInputs());
    }

    void Save(File& fstream) const override
    {
        Base::Save(fstream);
        uint32_t imageLayoutKind = (uint32_t)m_imageLayoutKind;
        uint32_t windowWidth = (uint32_t)m_windowWidth;
        fstream << windowWidth << imageLayoutKind << m_windowHeight << m_horizontalSubsample << m_verticalSubsample;
    }

    void Load(File& fstream, size_t modelVersion) override
    {
        Base::Load(fstream, modelVersion);
        uint32_t imageLayoutKind, windowWidth;
        fstream >> windowWidth >> imageLayoutKind >> m_windowHeight >> m_horizontalSubsample >> m_verticalSubsample;
        m_windowWidth = windowWidth;
        m_imageLayoutKind = (ImageLayoutKind)imageLayoutKind;

        ConvertToTensorShape();
    }

    void CopyTo(ComputationNodeBasePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const override
    {
        Base::CopyTo(nodeP, newName, flags);
        if (flags & CopyNodeFlags::copyNodeValue)
        {
            auto node = dynamic_pointer_cast<PoolingNodeBase<ElemType>>(nodeP);

            node->m_windowWidth = m_windowWidth;
            node->m_windowHeight = m_windowHeight;

            node->m_horizontalSubsample = m_horizontalSubsample;
            node->m_verticalSubsample = m_verticalSubsample;

            node->m_inputSizePerSample = m_inputSizePerSample;
            node->m_outputSizePerSample = m_outputSizePerSample;

            node->m_imageLayoutKind = m_imageLayoutKind;

            node->ConvertToTensorShape();
        }
    }

    void ForwardProp(const FrameRange& fr) override
    {
        Matrix<ElemType> sliceInput0Value = InputRef(0).ValueFor(fr);
        Matrix<ElemType> sliceOutputValue = ValueFor(fr);

        m_convEng->ForwardPooling(sliceInput0Value, sliceOutputValue);
    }

    void BackpropTo(const size_t /*inputIndex*/, const FrameRange& fr) override
    {
        Matrix<ElemType> sliceInput0Grad = InputRef(0).GradientFor(fr);
        Matrix<ElemType> sliceOutputGrad = GradientFor(fr);

        Matrix<ElemType> sliceInput0Value = InputRef(0).ValueFor(fr);
        Matrix<ElemType> sliceOutputValue = ValueFor(fr);

        m_convEng->BackwardPooling(sliceOutputValue, sliceOutputGrad, sliceInput0Value, sliceInput0Grad, !InputRef(0).IsGradientInitializedBy(this));
    }

    virtual ParentGradientOptimization ImplementsGradientOptimization(const ComputationNodeBase*) const override
    {
        return ParentGradientOptimization::Overwrite;
    }

    void Validate(bool isFinalValidationPass) override
    {
        Base::Validate(isFinalValidationPass);
        InferMBLayoutFromInputsForStandardCase(isFinalValidationPass);

        // get input tensor shape and interpret as image dimensions
        auto inDims = ImageDimensions(GetInputSampleLayout(0), m_imageLayoutKind);

        if (isFinalValidationPass && (inDims.m_width < m_windowWidth || inDims.m_height < m_windowHeight))
            InvalidArgument("PoolingNodeBase: inputWidth must >= windowWidth and inputHeight must >= windowHeight.");

        // determine output tensor shape
        auto outDims = ImageDimensions(
            (inDims.m_width - m_windowWidth) / m_horizontalSubsample + 1,
            (inDims.m_height - m_windowHeight) / m_verticalSubsample + 1,
            inDims.m_numChannels);

        m_inputSizePerSample = inDims.m_width * inDims.m_height * inDims.m_numChannels;

        SetDims(outDims.AsTensorShape(m_imageLayoutKind), HasMBLayout());

        if (isFinalValidationPass)
        {
            // set up various engines and descriptor objects
            m_geometry = std::make_shared<ConvolveGeometry>(inDims.AsTensorShape(m_imageLayoutKind),
                                                            ImageDimensions(m_windowWidth, m_windowHeight, 1).AsTensorShape(m_imageLayoutKind),
                                                            TensorShape(1),
                                                            ImageDimensions(m_horizontalSubsample, m_verticalSubsample, 1).AsTensorShape(m_imageLayoutKind),
                                                            ConvolveGeometry::BoolVec{true},
                                                            ConvolveGeometry::BoolVec{false},
                                                            TensorShape(0),
                                                            TensorShape(0));
        }
    }

    void DumpNodeInfo(const bool printValues, const bool printMetadata, File& fstream) const override
    {
        Base::DumpNodeInfo(printValues, printMetadata, fstream);

        if (printMetadata)
        {
            auto inputSampleLayout = GetInputSampleLayout(0);

            char str[4096];
            sprintf(str, "Input[Width:%lu, Height:%lu, Channels:%lu]  \n", (unsigned long)inputSampleLayout[1], (unsigned long)inputSampleLayout[2], (unsigned long)inputSampleLayout[0]);
            fstream << string(str);
            sprintf(str, "PoolingWindow[Width:%lu, Height:%lu]  SubSampling[Horizontal:%lu, Vertical:%lu]\n", (unsigned long)m_windowWidth, (unsigned long)m_windowHeight, (unsigned long)m_horizontalSubsample, (unsigned long)m_verticalSubsample);
            fstream << string(str);
            sprintf(str, "Output[Width:%lu, Height:%lu, Channels:%lu]  \n", (unsigned long)m_sampleLayout[1], (unsigned long)m_sampleLayout[2], (unsigned long)m_sampleLayout[0]);
            fstream << string(str);
            sprintf(str, "TotalSizePerSample[Input:%lu, Output:%lu]  \n", (unsigned long)m_inputSizePerSample, (unsigned long)m_outputSizePerSample);
            fstream << string(str);
        }
    }

    bool IsImageLayoutCHW() const { return m_imageLayoutKind == ImageLayoutKind::CHW; }
    TensorShape KernelShape() const { return m_kernelShape; }
    TensorShape Strides() const { return m_stride; }
    std::vector<bool> Sharing() const { return m_sharing; }
    std::vector<bool> AutoPad() const { return m_autoPad; }
    TensorShape LowerPad() const { return m_lowerPad; }
    TensorShape UpperPad() const { return m_upperPad; }
    PoolKind PoolingKind() const { return m_poolKind; }

protected:
    void ConvertToTensorShape()
    {
        m_kernelShape = ImageDimensions(m_windowWidth, m_windowHeight, 1).AsTensorShape(m_imageLayoutKind);
        m_stride      = ImageDimensions(m_horizontalSubsample, m_verticalSubsample, 1).AsTensorShape(m_imageLayoutKind);
        m_sharing     = { true };
        m_autoPad     = { false };
        m_lowerPad    = TensorShape(0);
        m_upperPad    = TensorShape(0);
    }

protected:
    size_t m_windowWidth, m_windowHeight;
    size_t m_horizontalSubsample, m_verticalSubsample;
    size_t m_inputSizePerSample, m_outputSizePerSample;

    ImageLayoutKind m_imageLayoutKind; // how to interpret the tensor (which dimensions are X/Y and C)

    // Mapping to V2 PoolingNode description..
    PoolKind m_poolKind;
    TensorShape m_kernelShape;
    TensorShape m_stride;
    std::vector<bool> m_sharing;
    std::vector<bool> m_autoPad;
    TensorShape m_lowerPad;
    TensorShape m_upperPad;

    ConvolveGeometryPtr m_geometry;
    std::unique_ptr<ConvolutionEngine<ElemType>> m_convEng;
};

// add this at the start of each derived class, to get access to the members of ComputationNode
// See #define of 'UsingComputationNodeMembersBoilerplate' for more explanation.
#define UsingPoolingNodeBaseMembers         \
    UsingComputationNodeMembersBoilerplate; \
    \
protected:                                  \
    using Base::m_geometry;                 \
    using Base::m_convEng;                  \
    using Base::m_windowWidth;              \
    using Base::m_windowHeight;             \
    using Base::m_horizontalSubsample;      \
    using Base::m_verticalSubsample;        \
    using Base::m_inputSizePerSample;       \
    using Base::m_outputSizePerSample;      \
    using Base::m_imageLayoutKind;          \
    \
public:

// -----------------------------------------------------------------------
// Legacy MaxPoolingNode
// -----------------------------------------------------------------------

template <class ElemType>
class MaxPoolingNode : public PoolingNodeBase<ElemType>
{
    typedef PoolingNodeBase<ElemType> Base;
    UsingPoolingNodeBaseMembers;
    static const std::wstring TypeName()
    {
        return L"MaxPooling";
    }

public:
    MaxPoolingNode(DEVICEID_TYPE deviceId, const wstring& name)
        : Base(deviceId, name, PoolKind::Max)
    {
    }
    MaxPoolingNode(DEVICEID_TYPE deviceId, const wstring& name, const size_t windowWidth, const size_t windowHeight, const size_t horizontalSubsample, const size_t verticalSubsample, ImageLayoutKind imageLayoutKind)
        : Base(deviceId, name, windowWidth, windowHeight, horizontalSubsample, verticalSubsample, imageLayoutKind, PoolKind::Max)
    {
    }
    MaxPoolingNode(const ScriptableObjects::IConfigRecordPtr configp)
        : Base(configp, PoolKind::Max)
    {
    }

    void Validate(bool isFinalValidationPass) override
    {
        Base::Validate(isFinalValidationPass);
        if (isFinalValidationPass && m_convEng == nullptr)
        {
            m_convEng = ConvolutionEngine<ElemType>::Create(m_geometry, m_deviceId, m_imageLayoutKind,
                                                            0, PoolKind::Max,
                                                            ConvolutionEngineKind::All, NodeName());
        }
    }
};

// -----------------------------------------------------------------------
// Legacy AveragePoolingNode
// -----------------------------------------------------------------------

template <class ElemType>
class AveragePoolingNode : public PoolingNodeBase<ElemType>
{
    typedef PoolingNodeBase<ElemType> Base;
    UsingPoolingNodeBaseMembers;
    static const std::wstring TypeName()
    {
        return L"AveragePooling";
    }

public:
    AveragePoolingNode(DEVICEID_TYPE deviceId, const wstring& name)
        : Base(deviceId, name, PoolKind::Average)
    {
    }
    AveragePoolingNode(DEVICEID_TYPE deviceId, const wstring& name, const size_t windowWidth, const size_t windowHeight, const size_t horizontalSubsample, const size_t verticalSubsample, ImageLayoutKind imageLayoutKind)
        : Base(deviceId, name, windowWidth, windowHeight, horizontalSubsample, verticalSubsample, imageLayoutKind, PoolKind::Average)
    {
    }
    AveragePoolingNode(const ScriptableObjects::IConfigRecordPtr configp)
        : Base(configp, PoolKind::Average)
    {
    }

    void Validate(bool isFinalValidationPass) override
    {
        Base::Validate(isFinalValidationPass);
        if (isFinalValidationPass && m_convEng == nullptr)
        {
            m_convEng = ConvolutionEngine<ElemType>::Create(m_geometry, m_deviceId, m_imageLayoutKind,
                                                            0, PoolKind::Average,
                                                            ConvolutionEngineKind::All, NodeName());
        }
    }
};

} } }
