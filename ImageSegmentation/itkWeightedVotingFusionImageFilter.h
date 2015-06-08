/*=========================================================================
 *
 *  Copyright Insight Software Consortium
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0.txt
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *=========================================================================*/
#ifndef __itkWeightedVotingFusionImageFilter_h
#define __itkWeightedVotingFusionImageFilter_h

#include "itkImageToImageFilter.h"

#include "itkConstNeighborhoodIterator.h"
#include "itkSimpleFastMutexLock.h"

#include <vnl/vnl_matrix.h>
#include <vnl/vnl_vector.h>

#include <vector>
#include <map>
#include <set>

namespace itk
{

/** \class WeightedVotingFusionImageFilter
 * \brief Implementation of the MALF algorithm.
 *
 * \author Paul Yushkevich
 *
 * \par REFERENCE
 *
 * H. Wang, J. W. Suh, S. Das, J. Pluta, C. Craige, P. Yushkevich,
 * "Multi-atlas segmentation with joint label fusion," IEEE Trans.
 * on Pattern Analysis and Machine Intelligence, 35(3), 611-623, 2013.
 *
 * H. Wang and P. A. Yushkevich, "Multi-atlas segmentation with joint
 * label fusion and corrective learning--an open source implementation,"
 * Front. Neuroinform., 2013.
 *
 * \ingroup ImageSegmentation
 */

template <class TInputImage, class TOutputImage>
class WeightedVotingFusionImageFilter
: public ImageToImageFilter<TInputImage, TOutputImage>
{
public:
  /** Standard class typedefs. */
  typedef WeightedVotingFusionImageFilter                 Self;
  typedef ImageToImageFilter<TInputImage, TOutputImage>   Superclass;
  typedef SmartPointer<Self>                              Pointer;
  typedef SmartPointer<const Self>                        ConstPointer;

  /** Run-time type information (and related methods). */
  itkTypeMacro( WeightedVotingFusionImageFilter, ImageToImageFilter );

  itkNewMacro( Self );

  /** ImageDimension constants */
  itkStaticConstMacro( ImageDimension, unsigned int, TInputImage::ImageDimension );


  /** Some convenient typedefs. */
  typedef TInputImage                                InputImageType;
  typedef typename InputImageType::Pointer           InputImagePointer;
  typedef typename InputImageType::ConstPointer      InputImageConstPointer;
  typedef typename InputImageType::PixelType         InputImagePixelType;
  typedef std::vector<InputImagePointer>             InputImageList;
  typedef std::vector<InputImageList>                InputImageSetList;

  typedef std::vector<InputImagePixelType>           InputImagePixelVectorType;

  typedef TOutputImage                               OutputImageType;

  typedef typename InputImageType::RegionType        RegionType;
  typedef typename InputImageType::SizeType          SizeType;
  typedef typename InputImageType::IndexType         IndexType;

  typedef unsigned int                               LabelType;
  typedef std::set<LabelType>                        LabelSetType;
  typedef Image<LabelType, ImageDimension>           LabelImageType;
  typedef typename LabelImageType::Pointer           LabelImagePointer;
  typedef std::vector<LabelImagePointer>             LabelImageList;

  typedef Image<float, ImageDimension>               ProbabilityImageType;
  typedef typename ProbabilityImageType::Pointer     ProbabilityImagePointer;

  typedef double                                     RealType;
  typedef std::vector<int>                           OffsetList;

  typedef vnl_matrix<RealType>                       MatrixType;
  typedef vnl_vector<RealType>                       VectorType;

  typedef std::map<LabelType, ProbabilityImagePointer>  LabelPosteriorProbabilityMap;
  typedef std::map<LabelType, LabelImagePointer>        LabelExclusionMap;
  typedef std::vector<ProbabilityImagePointer>          VotingWeightImageList;

  typedef ConstNeighborhoodIterator<InputImageType>               ConstNeighborhoodIteratorType;
  typedef typename ConstNeighborhoodIteratorType::RadiusType      NeighborhoodRadiusType;
  typedef typename ConstNeighborhoodIteratorType::OffsetType      NeighborhoodOffsetType;

  /**
   * Set the multimodal target image
   */
  void SetTargetImage( InputImageList imageList )
    {
    this->m_TargetImage = imageList;

    if( this->m_NumberOfModalities == 0 )
      {
      itkDebugMacro( "Setting the number of modalities to " << this->m_NumberOfModalities << "." );
      this->m_NumberOfModalities = imageList.size();
      }
    else if( this->m_NumberOfModalities != imageList.size() )
      {
      itkExceptionMacro( "The number of target multimodal images is not equal to " <<  this->m_NumberOfModalities );
      }
    this->UpdateInputs();
    }

  /**
   * Add an atlas (multi-modal image + segmentation)
   */
  void AddAtlas( InputImageList imageList, LabelImageType *segmentation = ITK_NULLPTR )
    {
    for( unsigned int i = 0; i < imageList.size(); i++ )
      {
      this->m_AtlasImages.push_back( imageList );
      }
    if( this->m_NumberOfModalities == 0 )
      {
      itkDebugMacro( "Setting the number of modalities to " << this->m_NumberOfModalities );
      this->m_NumberOfModalities = imageList.size();
      }
    else if( this->m_NumberOfModalities != imageList.size() )
      {
      itkExceptionMacro( "The number of atlas multimodal images is not equal to " <<  this->m_NumberOfModalities );
      }
    this->m_NumberOfAtlases++;

    if( segmentation != ITK_NULLPTR )
      {
      this->m_AtlasSegmentations.push_back( segmentation );
      this->m_NumberOfAtlasSegmentations++;
      }

    this->UpdateInputs();
    }

  /**
   * Add a label exclusion map
   */
  void AddLabelExclusionImage( LabelType label, LabelImageType *exclusionImage )
    {
    this->m_LabelExclusionImages[label] = exclusionImage;
    this->UpdateInputs();
    }

  /**
   * Set the number of modalities used in determining the optimal label fusion
   * or optimal fused image.
   */
  itkSetMacro( NumberOfModalities, unsigned int );
  itkGetConstMacro( NumberOfModalities, unsigned int );

  /**
   * Set/Get the local search neighborhood for minimizing potential registration error.
   * Default = 3x3x3.
   */
  itkSetMacro( SearchNeighborhoodRadius, NeighborhoodRadiusType );
  itkGetConstMacro( SearchNeighborhoodRadius, NeighborhoodRadiusType );

  /**
   * Set/Get the patch neighborhood for calculating the similarity measures.
   * Default = 2x2x2.
   */
  itkSetMacro( PatchNeighborhoodRadius, NeighborhoodRadiusType );
  itkGetConstMacro( PatchNeighborhoodRadius, NeighborhoodRadiusType );

  /**
   * Set/Get the Alpha parameter---the regularization weight added to the matrix Mx for
   * the inverse.  Default = 0.1.
   */
  itkSetMacro( Alpha, RealType );
  itkGetConstMacro( Alpha, RealType );

  /**
   * Set/Get the Beta parameter---exponent for mapping intensity difference to joint error.
   * Default = 2.0.
   */
  itkSetMacro( Beta, RealType );
  itkGetConstMacro( Beta, RealType );

  /** Set the requested region */
  void GenerateInputRequestedRegion() ITK_OVERRIDE;

  /**
   * Boolean for retaining the posterior images. This can have a negative effect
   * on memory use, so it should only be done if one wishes to save the posterior
   * maps. The posterior maps (n = number of labels) give the probability of each
   * voxel in the target image belonging to each label.  Default = false.
   */
  itkSetMacro( RetainLabelPosteriorProbabilityImages, bool );
  itkGetConstMacro( RetainLabelPosteriorProbabilityImages, bool );
  itkBooleanMacro( RetainLabelPosteriorProbabilityImages );

  /**
   * Boolean for retaining the voting weights images.  This can have a negative effect
   * on memory use, so it should only be done if one wishes to save the voting weight
   * maps.  The voting weight maps (n = number of atlases) gives the contribution of
   * a particular atlas to the final label/intensity fusion.
   */
  itkSetMacro( RetainAtlasVotingWeightImages, bool );
  itkGetConstMacro( RetainAtlasVotingWeightImages, bool );

  /**
   * Get the posterior probability image corresponding to a label.
   */
  const ProbabilityImagePointer GetLabelPosteriorProbabilityImage( LabelType label )
    {
    if( this->m_RetainLabelPosteriorProbabilityImages )
      {
      if( std::find( this->m_LabelSet.begin(), this->m_LabelSet.end(), label ) != this->m_LabelSet.end() )
        {
        return this->m_LabelPosteriorProbabilityImages[label];
        }
      else
        {
        itkDebugMacro( "Not returning a label posterior probability image.  Requested label not found." );
        return ITK_NULLPTR;
        }
      }
    else
      {
      itkDebugMacro( "Not returning a label posterior probability image.  These images were not saved." );
      return ITK_NULLPTR;
      }
    }

  /**
   * Get the voting weight image corresponding to an atlas.
   */
  const ProbabilityImagePointer GetAtlasVotingWeightImage( unsigned int n )
    {
    if( this->m_RetainAtlasVotingWeightImages )
      {
      if( n < this->m_NumberOfAtlases )
        {
        return this->m_AtlasVotingWeightImages[n];
        }
      else
        {
        itkDebugMacro( "Not returning a voting weight image.  Requested index is greater than the number of atlases." );
        return ITK_NULLPTR;
        }
      }
    else
      {
      itkDebugMacro( "Not returning a voting weight image.  These images were not saved." );
      return ITK_NULLPTR;
      }
    }

protected:

  WeightedVotingFusionImageFilter();
  ~WeightedVotingFusionImageFilter() {}

  void PrintSelf( std::ostream& os, Indent indent ) const ITK_OVERRIDE;

  void ThreadedGenerateData( const RegionType &, ThreadIdType ) ITK_OVERRIDE;

  void BeforeThreadedGenerateData() ITK_OVERRIDE;

  void AfterThreadedGenerateData() ITK_OVERRIDE;

private:

  RealType ComputePatchSimilarity( const InputImagePixelVectorType &, const InputImagePixelVectorType & );

  InputImagePixelVectorType VectorizeImageListPatch( const InputImageList &, const IndexType, const bool );

  InputImagePixelVectorType VectorizeImagePatch( const InputImagePointer, const IndexType, const bool );

  void GetMeanAndStandardDeviationOfVectorizedImagePatch( const InputImagePixelVectorType &, RealType &, RealType & );

  void UpdateInputs();

  /** Input variables   */
  InputImageList                                       m_TargetImage;
  InputImageSetList                                    m_AtlasImages;
  LabelImageList                                       m_AtlasSegmentations;
  LabelExclusionMap                                    m_LabelExclusionImages;

  LabelSetType                                         m_LabelSet;
  SizeValueType                                        m_NumberOfAtlases;
  SizeValueType                                        m_NumberOfAtlasSegmentations;
  SizeValueType                                        m_NumberOfModalities;

  NeighborhoodRadiusType                               m_SearchNeighborhoodRadius;
  NeighborhoodRadiusType                               m_PatchNeighborhoodRadius;
  SizeValueType                                        m_SearchNeighborhoodSize;
  SizeValueType                                        m_PatchNeighborhoodSize;
  std::vector<NeighborhoodOffsetType>                  m_SearchNeighborhoodOffsetList;

  RealType                                             m_Alpha;
  RealType                                             m_Beta;

  bool                                                 m_RetainLabelPosteriorProbabilityImages;
  bool                                                 m_RetainAtlasVotingWeightImages;

  ProbabilityImagePointer                              m_WeightSumImage;

  /** Output variables     */
  LabelPosteriorProbabilityMap                         m_LabelPosteriorProbabilityImages;
  VotingWeightImageList                                m_AtlasVotingWeightImages;

  InputImageList                                       m_JointIntensityFusionImage;

  SimpleFastMutexLock                                  m_MutexLock;
};

} // namespace itk

#ifndef ITK_MANUAL_INSTANTIATION
#include "itkWeightedVotingFusionImageFilter.hxx"
#endif

#endif
