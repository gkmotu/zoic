// ZOIC - Extended Arnold camera shader with options for:
// Refracting through lens elements read from ground truth lens data
        // Physically plausible lens distortion and optical vignetting
// Image based bokeh shapes
// Emperical optical vignetting using the thin-lens equation

// (C) Zeno Pelgrims, www.zenopelgrims.com/zoic

// Calculate proper ray derivatives for optimal texture i/o
// Add support for C4D (IDs need to be generated)
// Test Houdini support
// Make visualisation for all parameters for website
 

#include <ai.h>
#include <iostream>
#include <string>
#include <cstring>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <map>
#include <iterator>
#include <algorithm>


#ifdef _MACBOOK
#  define MACBOOK_ONLY(block) block
#else
#  define MACBOOK_ONLY(block)
#endif

#ifdef _WORK
#  define WORK_ONLY(block) block
#else
#  define WORK_ONLY(block)
#endif

#ifdef _DEBUGIMAGESAMPLING
#  define DEBUG_ONLY(block) block
#else
#  define DEBUG_ONLY(block)
#endif
 
#ifdef _DRAW
#  define DRAW_ONLY(block) block
#else
#  define DRAW_ONLY(block)
#endif


#ifdef NO_OIIO
// AiTextureLoad function introduced in arnold 4.2.9.0 was modified in 4.2.10.0
// Figure out the right one to call at compile time
#  if AI_VERSION_ARCH_NUM > 4
#   define AITEXTURELOAD_PROTO2
#  else
#    if AI_VERSION_ARCH_NUM == 4
#      if AI_VERSION_MAJOR_NUM > 2
#        define AITEXTURELOAD_PROTO2
#      else
#        if AI_VERSION_MAJOR_NUM == 2
#          if AI_VERSION_MINOR_NUM >= 10
#            define AITEXTURELOAD_PROTO2
#          endif
#          if AI_VERSION_MINOR_NUM == 9
#            define AITEXTURELOAD_PROTO1
#          endif
#        endif
#      endif
#    endif
#  endif
#  ifdef AITEXTURELOAD_PROTO2
inline bool LoadTexture(const AtString path, void *pixelData){
    return AiTextureLoad(path, true, 0, pixelData);
}
#  else
#    ifdef AITEXTURELOAD_PROTO1
inline bool LoadTexture(const AtString path, void *pixelData){
    return AiTextureLoad(path, true,  pixelData);
}
#    else
inline bool LoadTexture(const AtString path, void *pixelData){
    AiMsgError("Current arnold version doesn't have texture loading API");
    return false;
}
#    endif
#  endif
#else
#  include <OpenImageIO/imageio.h>
#endif

 
// global vars for lens drawing, remove these at some point
std::ofstream myfile;
std::ofstream testAperturesFile;
bool draw = false;
int counter = 0;

 
// Arnold methods
AI_CAMERA_NODE_EXPORT_METHODS(zoicMethods)


enum zoicParams{
    p_sensorWidth,
    p_sensorHeight,
    p_focalLength,
    p_fStop,
    p_focalDistance,
    p_useImage,
    p_bokehPath,
    p_lensModel,
    p_lensDataPath,
    p_kolbSamplingLUT,
    p_useDof,
    p_opticalVignettingDistance,
    p_opticalVignettingRadius,
    p_exposureControl
};


enum LensModel{
    THINLENS,
    RAYTRACED,
    NONE
};


static const char* LensModelNames[] = 
{
    "THINLENS",
    "RAYTRACED",
    NULL
};


struct arrayCompare{
    const float *values;
    inline arrayCompare(const float *_values) :values(_values) {}
    inline bool operator()(int _lhs, int _rhs) const{
        return values[_lhs] > values[_rhs];
    }
};

class imageData{
private:
    int x, y;
    int nchannels;
    float *pixelData;
    float *cdfRow;
    float *cdfColumn;
    int *rowIndices;
    int *columnIndices;
    
public:
    imageData()
        : x(0), y(0), nchannels(0)
        , pixelData(0), cdfRow(0), cdfColumn(0)
        , rowIndices(0), columnIndices(0) {
    }
     
    ~imageData(){
        invalidate();
    }
     
    bool isValid() const{
        return (x * y * nchannels > 0 && nchannels >= 3);
    }
     
    void invalidate(){
        if (pixelData){
            AiAddMemUsage(-x * y * nchannels * sizeof(float), "zoic");
            AiFree(pixelData);
            pixelData = 0;
        }
        if (cdfRow){
            AiAddMemUsage(-y * sizeof(float), "zoic");
            AiFree(cdfRow);
            cdfRow = 0;
        }
        if (cdfColumn){
            AiAddMemUsage(-x * y * sizeof(float), "zoic");
            AiFree(cdfColumn);
            cdfColumn = 0;
        }
        if (rowIndices){
            AiAddMemUsage(-y * sizeof(int), "zoic");
            AiFree(rowIndices);
            rowIndices = 0;
        }
        if (columnIndices){
            AiAddMemUsage(-x * y * sizeof(int), "zoic");
            AiFree(columnIndices);
            columnIndices = 0;
        }
        x = y = nchannels = 0;
    }
    
    bool read(const char *bokeh_kernel_filename){
        
        invalidate();
        
        AtInt64 nbytes = 0;
        
#ifdef NO_OIIO

        AiMsgInfo("Reading image using Arnold API: %s", bokeh_kernel_filename);

        AtString path(bokeh_kernel_filename);

        unsigned int iw, ih, nc;
        if (!AiTextureGetResolution(path, &iw, &ih) ||
            !AiTextureGetNumChannels(path, &nc)){
            return false;
        }

        x = int(iw);
        y = int(ih);
        nchannels = int(nc);
        
        nbytes = x * y * nchannels * sizeof(float);
        AiAddMemUsage(nbytes, "zoic");
        pixelData = (float*) AiMalloc(nbytes);

        if (!LoadTexture(path, pixelData)){
            invalidate();
            return false;
        }

#else

        AiMsgInfo("Reading image using OpenImageIO: %s", bokeh_kernel_filename);

        //Search for an ImageIO plugin that is capable of reading the file ("foo.jpg"), first by
        //trying to deduce the correct plugin from the file extension, but if that fails, by opening
        //every ImageIO plugin it can find until one will open the file without error. When it finds
        //the right plugin, it creates a subclass instance of ImageInput that reads the right kind of
        //file format, and tries to fully open the file.
        OpenImageIO::ImageInput *in = OpenImageIO::ImageInput::open (bokeh_kernel_filename);
        if (! in){
            return false;
        }

        const OpenImageIO::ImageSpec &spec = in->spec();
        
        x = spec.width;
        y = spec.height;
        nchannels = spec.nchannels;

        nbytes = x * y * nchannels * sizeof(float);
        AiAddMemUsage(nbytes, "zoic");
        pixelData = (float*) AiMalloc(nbytes);

        in->read_image(OpenImageIO::TypeDesc::FLOAT, pixelData);
        in->close();
        delete in;

#endif

        AiMsgInfo("Image Width: %d", x);
        AiMsgInfo("Image Height: %d", y);
        AiMsgInfo("Image Channels: %d", nchannels);
        AiMsgInfo("Total amount of pixels to process: %d", x * y);

        DEBUG_ONLY({
            // print out raw pixel data
            int npixels = x * y;
            for (int i = 0, j = 0; i < npixels; i++){
                std::cout << "[";
                for (int k = 0; k < nchannels; k++, j++){
                    std::cout << pixelData[j];
                    if (k + 1 < nchannels){
                        std::cout << ", ";
                    }
                }
                std::cout << "]";
                if (i + 1 < npixels){
                    std::cout << ", ";
                }
            }
            std::cout << "----------------------------------------------" << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
        })
        
        bokehProbability();
        
        return true;
    }
    
    // Importance sampling
    void bokehProbability(){
        if (!isValid()){
            return;
        }
        
        // initialize arrays
        AtInt64 nbytes = x * y * sizeof(float);
        AtInt64 totalTempBytes = 0;
        
        AiAddMemUsage(nbytes, "zoic");    
        float *pixelValues = (float*) AiMalloc(nbytes);
        totalTempBytes += nbytes;
        
        AiAddMemUsage(nbytes, "zoic");
        float *normalizedPixelValues = (float*) AiMalloc(nbytes);
        totalTempBytes += nbytes;
        
        int npixels = x * y;
        int o1 = (nchannels >= 2 ? 1 : 0);
        int o2 = (nchannels >= 3 ? 2 : o1);
        float totalValue = 0.0f;
        
        // for every pixel, stuff going wrong here
        for (int i=0, j=0; i < npixels; ++i, j+=nchannels){
            // store pixel value in array
            // calculate luminance [Y = 0.3 R + 0.59 G + 0.11 B]
            pixelValues[i] = pixelData[j] * 0.3f + pixelData[j+o1] * 0.59f + pixelData[j+o2] * 0.11f;
            
            totalValue += pixelValues[i];
            
            DEBUG_ONLY(std::cout << "Pixel Luminance: " << i << " -> " << pixelValues[i] << std::endl);
        }
        
        DEBUG_ONLY({
            std::cout << "----------------------------------------------" << std::endl;
            std::cout << "DEBUG: Total Pixel Value: " << totalValue << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
        })

        // normalize pixel values so sum = 1
        float invTotalValue = 1.0f / totalValue;
        float totalNormalizedValue = 0.0f;
        
        for(int i=0; i < npixels; ++i){
            normalizedPixelValues[i] = pixelValues[i] * invTotalValue;
            
            totalNormalizedValue += normalizedPixelValues[i];
            
            DEBUG_ONLY(std::cout << "Normalized Pixel Value: " << i << ": " << normalizedPixelValues[i] << std::endl);
        }

        DEBUG_ONLY({
            std::cout << "----------------------------------------------" << std::endl;
            std::cout << "DEBUG: Total Normalized Pixel Value: " << totalNormalizedValue << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
        })

        // calculate sum for each row
        nbytes = y * sizeof(float);
        AiAddMemUsage(nbytes, "zoic");
        float *summedRowValues = (float*) AiMalloc(nbytes);
        totalTempBytes += nbytes;

        for(int i=0, k=0; i < y; ++i){

            summedRowValues[i] = 0.0f;

            for(int j=0; j < x; ++j, ++k){

                summedRowValues[i] += normalizedPixelValues[k];
            }
            
            DEBUG_ONLY(std::cout << "Summed Values row [" << i << "]: " << summedRowValues[i] << std::endl);
        }


        DEBUG_ONLY({
            // calculate sum of all row values, just to debug
            float totalNormalizedRowValue = 0.0f;
            for(int i=0; i < y; ++i){
                totalNormalizedRowValue += summedRowValues[i];
            }
            std::cout << "----------------------------------------------" << std::endl;
            std::cout << "Debug: Summed Row Value: " << totalNormalizedRowValue << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
        })


        // make array of indices
        nbytes = y * sizeof(int);
        AiAddMemUsage(nbytes, "zoic");
        rowIndices = (int*) AiMalloc(nbytes);

        for(int i = 0; i < y; ++i){
            rowIndices[i] = i;
        }

        std::sort(rowIndices, rowIndices + y, arrayCompare(summedRowValues));


        DEBUG_ONLY({
            // print values
            for(int i = 0; i < y; ++i){
                std::cout << "PDF row [" <<  rowIndices[i] << "]: " << summedRowValues[rowIndices[i]] << std::endl;
            }
            std::cout << "----------------------------------------------" << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
        })


        // For every row, add the sum of all previous row (cumulative distribution function)
        nbytes = y * sizeof(float);
        AiAddMemUsage(nbytes, "zoic");
        cdfRow = (float*) AiMalloc(nbytes);

        float prevVal = 0.0f;

        for (int i = 0; i < y; ++i){
            cdfRow[i] = prevVal + summedRowValues[rowIndices[i]];
            prevVal = cdfRow[i];

            DEBUG_ONLY(std::cout << "CDF row [" << rowIndices[i] << "]: " << cdfRow[i] << std::endl);
        }

        DEBUG_ONLY({
            std::cout << "----------------------------------------------" << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
        })
        
        nbytes = npixels * sizeof(float);
        AiAddMemUsage(nbytes, "zoic");
        float *normalizedValuesPerRow = (float*) AiMalloc(nbytes);
        totalTempBytes += nbytes;
        
        // divide pixel values of each pixel by the sum of the pixel values of that row (Normalize)
        for (int r = 0, i = 0; r < y; ++r){
            for (int c = 0; c < x; ++c, ++i){
                // avoid division by 0
                if ((normalizedPixelValues[i] != 0) && (summedRowValues[r] != 0)){
                    normalizedValuesPerRow[i] = normalizedPixelValues[i] / summedRowValues[r];
                }
                else{
                    normalizedValuesPerRow[i] = 0;
                }

                DEBUG_ONLY(std::cout << "Normalized Pixel value per row: " << i << ": " << normalizedValuesPerRow[i] << std::endl);
            }
        }

        DEBUG_ONLY({
            std::cout << "----------------------------------------------" << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
        })

        // sort column values from highest to lowest per row (probability density function)
        nbytes = npixels * sizeof(int);
        AiAddMemUsage(nbytes, "zoic");
        columnIndices = (int*) AiMalloc(nbytes);
        
        for(int i = 0; i < npixels; i++){
            columnIndices[i] = i;
        }

        for (int i = 0; i < npixels; i+=x){
            std::sort(columnIndices + i, columnIndices + i + x, arrayCompare(normalizedValuesPerRow));
        }

        DEBUG_ONLY({
            // print values
            for(int i = 0; i < npixels; ++i){
                std::cout << "PDF column [" << columnIndices[i] << "]: " << normalizedValuesPerRow[columnIndices[i]] << std::endl;
            }
            std::cout << "----------------------------------------------" << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
        })

        // For every column per row, add the sum of all previous columns (cumulative distribution function)
        nbytes = npixels * sizeof(float);
        AiAddMemUsage(nbytes, "zoic");
        cdfColumn = (float*) AiMalloc(nbytes);
        
        for (int r = 0, i = 0; r < y; ++r){
            prevVal = 0.0f;
            
            for (int c = 0; c < x; ++c, ++i){
                cdfColumn[i] = prevVal + normalizedValuesPerRow[columnIndices[i]];
                prevVal = cdfColumn[i];

                DEBUG_ONLY(std::cout << "CDF column [" <<  columnIndices[i] << "]: " << cdfColumn[i] << std::endl);
            }
        }

        DEBUG_ONLY(std::cout << "----------------------------------------------" << std::endl);
            
        // Release and untrack memory
        AiAddMemUsage(-totalTempBytes, "zoic");
        
        AiFree(pixelValues);
        AiFree(normalizedPixelValues);
        AiFree(summedRowValues);
        AiFree(normalizedValuesPerRow);
    }
    
    // Sample image
    void bokehSample(float randomNumberRow, float randomNumberColumn, float *dx, float *dy){
        
        if (!isValid()){
            AiMsgWarning("Invalid bokeh image data.");
            *dx = 0.0f;
            *dy = 0.0f;
            return;
        }
        
        // print random number between 0 and 1
        DEBUG_ONLY(std::cout << "RANDOM NUMBER ROW: " << randomNumberRow << std::endl);

        // find upper bound of random number in the array
        float *pUpperBound = std::upper_bound(cdfRow, cdfRow + y, randomNumberRow);
        int r = 0;
        
        if (pUpperBound >= cdfRow + y){
            //AiMsgWarning("[zoic] %f larger than last biggest cdfRow[%d] = %f", randomNumberRow, y-1, cdfRow[y-1]);
            r = y - 1;
        
        } else{
            DEBUG_ONLY(std::cout << "UPPER BOUND: " << *pUpperBound << std::endl);
            r = int(pUpperBound - cdfRow);
        }
        

        // find actual pixel row
        int actualPixelRow = rowIndices[r];

        // recalculate pixel row so that the center pixel is (0,0) - might run into problems with images of dimensions like 2x2, 4x4, 6x6, etc
        int recalulatedPixelRow = actualPixelRow - ((x - 1) / 2);

        DEBUG_ONLY({
            // print values
            std::cout << "INDEX IN CDF ROW: " << r << std::endl;
            std::cout << "ACTUAL PIXEL ROW: " << actualPixelRow << std::endl;
            std::cout << "RECALCULATED PIXEL ROW: " << recalulatedPixelRow << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
            // print random number between 0 and 1
            std::cout << "RANDOM NUMBER COLUMN: " << randomNumberColumn << std::endl;
        })

        int startPixel = actualPixelRow * x;

        DEBUG_ONLY(std::cout << "START PIXEL: " << startPixel << std::endl);


        // find upper bound of random number in the array
        float *pUpperBoundColumn = std::upper_bound(cdfColumn + startPixel, cdfColumn + startPixel + x, randomNumberColumn);
        int c = 0;

        if (pUpperBoundColumn >= cdfColumn + startPixel + x){
            //AiMsgWarning("[zoic] %f larger than last biggest cdfColumn[%d][%d] = %f", randomNumberColumn, r, x-1, cdfColumn[startPixel+x-1]);
            c = startPixel + x - 1;

        } else{
            DEBUG_ONLY(std::cout << "UPPER BOUND: " << *pUpperBoundColumn << std::endl);
            c = int(pUpperBoundColumn - cdfColumn);
        }

        // find actual pixel column
        int actualPixelColumn = columnIndices[c];
        int relativePixelColumn = actualPixelColumn - startPixel;
        int recalulatedPixelColumn = relativePixelColumn - ((y - 1) / 2);

        DEBUG_ONLY({
            // print values
            std::cout << "INDEX IN CDF COLUMN: " << c << std::endl;
            std::cout << "ACTUAL PIXEL COLUMN: " << actualPixelColumn << std::endl;
            std::cout << "RELATIVE PIXEL COLUMN (starting from 0): " << relativePixelColumn << std::endl;
            std::cout << "RECALCULATED PIXEL COLUMN: " << recalulatedPixelColumn << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
        })

        // to get the right image orientation, flip the x and y coordinates and then multiply the y values by -1 to flip the pixels vertically
        float flippedRow = float(recalulatedPixelColumn);
        float flippedColumn = recalulatedPixelRow * -1.0f;

        // send values back
        *dx = (float)flippedRow / (float)x * 2;
        *dy = (float)flippedColumn / (float)y * 2;
    }
};

struct cameraData{
    float fov;
    float tan_fov;
    float apertureRadius;
    imageData image;
    
    cameraData()
        : fov(0.0f), tan_fov(0.0f), apertureRadius(0.0f){
    }
    
    ~cameraData(){
    }
};


class boundingBox2d{
public:
    AtPoint2 max, min;

    AtPoint2 getCentroid(){
        return {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f};
    }

    float getMaxScale(){
        AtPoint2 centroid = getCentroid();
        float scaleX = AiV2Dist(centroid, {max.x, centroid.y});
        float scaleY = AiV2Dist(centroid, {centroid.x, max.y});
        if (scaleX >= scaleY){return scaleX;} else {return scaleY;}
    }
};


struct LensElement{
public:
    float curvature, thickness, ior, aperture, abbe, center;
};
 
 
struct Lensdata{
    std::vector<LensElement> lenses;
    int lensCount;
    float userApertureRadius;
    int apertureElement;
    int vignettedRays, succesRays, drawRays;
    int totalInternalReflection;
    float apertureDistance;
    float focalLengthRatio;
    float filmDiagonal;
    float originShift;
    float focalDistance; 
    std::map<float, std::map<float, boundingBox2d>> apertureMap;
} ld;


struct LensdataCheckUpdate{
    float stored_sensorWidth = 0.0f;
    float stored_sensorHeight = 0.0f;
    float stored_focalLength = 0.0f;
    float stored_fStop = 0.0f;
    float stored_focalDistance = 0.0f;
    bool stored_useImage = false;
    std::string stored_bokehPath = "";
    std::string stored_lensDataPath = "";
    bool stored_kolbSamplingLUT = false;
} ldCheckUpdate;
 

// xorshift random number generator
uint32_t xor128(void) {
    static uint32_t x = 123456789, y = 362436069, z = 521288629, w = 88675123;
    uint32_t t = x ^ (x << 11);
    x = y; y = z; z = w;
    return w = (w ^ (w >> 19) ^ t ^ (t >> 8));
} 
 

// Improved concentric mapping code by Dave Cline [peter shirley´s blog]
inline void concentricDiskSample(float ox, float oy, AtPoint2 *lens) {
    float phi, r;

    // switch coordinate space from [0, 1] to [-1, 1]
    float a = 2.0 * ox - 1.0;
    float b = 2.0 * oy - 1.0;

    if (SQR(a) > SQR(b)){
        r = a;
        phi = (0.78539816339f) * (b / a);
    } else {
        r = b;
        phi = (AI_PIOVER2) - (0.78539816339f) * (a / b);
    }
        
    *lens = {r * std::cos(phi), r * std::sin(phi)};
}

 
void readTabularLensData(std::string lensDataFileName, Lensdata *ld){
    std::ifstream lensDataFile(lensDataFileName);
    std::string line, token;
    std::stringstream iss;
    int lensDataCounter = 0, commentCounter = 0;
    LensElement lens;
 
    AiMsgInfo("[ZOIC] ##############################################");
    AiMsgInfo("[ZOIC] ############# READING LENS DATA ##############");
    AiMsgInfo("[ZOIC] ##############################################");
    AiMsgInfo("[ZOIC] Welcome to the lens nerd club :-D");

    // read file without storing data, but count instead
    int columns = 0, lines = 0;

    while (getline(lensDataFile, line)){
        if (line.empty() || line.front() == '#'){continue;}
        std::size_t prev = 0, pos;
        iss << line;
 
        while ((pos = line.find_first_of("\t,;: ", prev)) != std::string::npos){
            if (pos > prev){++columns;}
            prev = pos + 1;
        }
        
        if (prev < line.length()){++columns;}

        iss.clear();
        ++lines;
    }

    lensDataFile.clear();
    lensDataFile.seekg(0, std::ios::beg);
    int totalColumns = static_cast<int>(static_cast<float>(columns) / static_cast<float>(lines));
    AiMsgInfo( "%-40s %12d", "[ZOIC] Data file columns", totalColumns);

    // bail out if not legal amount of columns
    if (totalColumns < 4){
        AiMsgError("[ZOIC] Failed to read lens data file.");
        AiMsgError("[ZOIC] Less than 4 columns of data are found. Please double check.");
        AiRenderAbort();
    } else if (totalColumns > 5){
        AiMsgError("[ZOIC] Failed to read lens data file.");
        AiMsgError("[ZOIC] More than 5 columns of data are found. Please double check.");
        AiRenderAbort();
    }

    // read in data
    switch(totalColumns)
    {
        case 4:
        {
            while (getline(lensDataFile, line)){
                if (line.empty() || line.front() == '#'){
                    ++commentCounter;
                    continue;
                }
         
                std::size_t prev = 0, pos;
                iss << line;
         
                while ((pos = line.find_first_of("\t,;: ", prev)) != std::string::npos){
                    if (pos > prev){
                        if (lensDataCounter == 0){
                            lens.curvature = std::stof(line.substr(prev, pos-prev));
                        } else if (lensDataCounter == 1){
                            lens.thickness = std::stof(line.substr(prev, pos-prev));
                        } else if (lensDataCounter == 2){
                            lens.ior = std::stof(line.substr(prev, pos-prev));
                        } else if (lensDataCounter == 3){
                            lens.aperture = std::stof(line.substr(prev, pos-prev));
                            lensDataCounter = -1;
                        }
                    }

                    prev = pos + 1;
                    ++lensDataCounter;
                }
                
                if (prev < line.length()){
                    if (lensDataCounter == 0){
                        lens.curvature = std::stof(line.substr(prev, std::string::npos));
                    } else if (lensDataCounter == 1){
                        lens.thickness = std::stof(line.substr(prev, std::string::npos));
                    } else if (lensDataCounter == 2){
                        lens.ior = std::stof(line.substr(prev, std::string::npos));
                    } else if (lensDataCounter == 3){
                        lens.aperture = std::stof(line.substr(prev, std::string::npos));
                        lensDataCounter = -1;
                    }

                    ++lensDataCounter;
                }

                ld->lenses.push_back(lens);
                iss.clear();
            }


            ld->lensCount = static_cast<int>(ld->lenses.size());
 
            AiMsgInfo( "%-40s %12d", "[ZOIC] Comment lines ignored", commentCounter);
         
            AiMsgInfo("[ZOIC] ##############################################");
            AiMsgInfo("[ZOIC] #   ROC       Thickness     IOR     Aperture #");
            AiMsgInfo("[ZOIC] ##############################################");
         
            for(int i = 0; i < ld->lensCount; i++){
                AiMsgInfo("[ZOIC] %10.4f  %10.4f  %10.4f  %10.4f", ld->lenses[i].curvature, ld->lenses[i].thickness, ld->lenses[i].ior, ld->lenses[i].aperture);
            }
         
            AiMsgInfo("[ZOIC] ##############################################");
            AiMsgInfo("[ZOIC] ########### END READING LENS DATA ############");
            AiMsgInfo("[ZOIC] ##############################################");

        } break;

        case 5:
        {
            while (getline(lensDataFile, line)){
                if (line.empty() || line.front() == '#'){
                    ++commentCounter;
                    continue;
                }
         
                std::size_t prev = 0, pos;
                iss << line;
         
                while ((pos = line.find_first_of("\t,;: ", prev)) != std::string::npos){
                    if (pos > prev){
                        if (lensDataCounter == 0){
                            lens.curvature = std::stof(line.substr(prev, pos-prev));
                        } else if (lensDataCounter == 1){
                            lens.thickness = std::stof(line.substr(prev, pos-prev));
                        } else if (lensDataCounter == 2){
                            lens.ior = std::stof(line.substr(prev, pos-prev));
                        } else if (lensDataCounter == 3){
                            lens.abbe = std::stof(line.substr(prev, pos-prev));
                        } else if (lensDataCounter == 4){
                            lens.aperture = std::stof(line.substr(prev, pos-prev));
                            lensDataCounter = -1;
                        }
                    }

                    prev = pos + 1;
                    ++lensDataCounter;
                }
                
                if (prev < line.length()){
                    if (lensDataCounter == 0){
                        lens.curvature = std::stof(line.substr(prev, std::string::npos));
                    } else if (lensDataCounter == 1){
                        lens.thickness = std::stof(line.substr(prev, std::string::npos));
                    } else if (lensDataCounter == 2){
                        lens.ior = std::stof(line.substr(prev, std::string::npos));
                    } else if (lensDataCounter == 3){
                            lens.abbe = std::stof(line.substr(prev, pos-prev));
                    } else if (lensDataCounter == 4){
                        lens.aperture = std::stof(line.substr(prev, std::string::npos));
                        lensDataCounter = -1;
                    }

                    ++lensDataCounter;
                }

                ld->lenses.push_back(lens);
                iss.clear();
            }


            ld->lensCount = static_cast<int>(ld->lenses.size());
 
            AiMsgInfo( "%-40s %12d", "[ZOIC] Comment lines ignored", commentCounter);
            AiMsgInfo("[ZOIC] ##############################################");
            AiMsgInfo("[ZOIC] #  ROC   Thickness   IOR    ABBE    Aperture #");
            AiMsgInfo("[ZOIC] ##############################################");
         
            for(int i = 0; i < ld->lensCount; i++){
                AiMsgInfo("[ZOIC] %7.3f  %7.3f %7.3f   %7.3f   %7.3f", ld->lenses[i].curvature, ld->lenses[i].thickness, ld->lenses[i].ior, ld->lenses[i].abbe, ld->lenses[i].aperture);
            }
         
            AiMsgInfo("[ZOIC] ##############################################");
            AiMsgInfo("[ZOIC] ########### END READING LENS DATA ############");
            AiMsgInfo("[ZOIC] ##############################################");

        } break;
    }
 
    // reverse the lens order, since we will start with the rear-most lens element
    std::reverse(ld->lenses.begin(),ld->lenses.end());
}
 
 
void cleanupLensData(Lensdata *ld){
    int apertureCount = 0;
    for (int i = 0; i < ld->lensCount; i++){
        // check if there is a 0.0 lensRadiusCurvature, which is the aperture
        if (ld->lenses[i].curvature == 0.0){
            ld->apertureElement = i;
            ++apertureCount;
 
            if(apertureCount > 1){
                AiMsgError("[ZOIC] Multiple apertures found. Provide lens description with 1 aperture.");
                AiRenderAbort();
            }
 
            AiMsgInfo("[ZOIC] Adjusted ROC[%d] [%.4f] to [99999.0]", i, ld->lenses[i].curvature);
            ld->lenses[i].curvature = 99999.0;
        }
 
        if (ld->lenses[i].ior == 0.0){
            AiMsgInfo("[ZOIC] Changed IOR[%d] [%.4f] to [1.0000]", i, ld->lenses[i].ior);
            ld->lenses[i].ior = 1.0;
        }
    }
 
    AiMsgInfo( "%-40s %12d", "[ZOIC] Aperture is lens element number", ld->apertureElement);
 
    // scale from mm to cm
    for (int i = 0; i < ld->lensCount; i++){
        ld->lenses[i].curvature *= 0.1;
        ld->lenses[i].thickness *= 0.1;
        ld->lenses[i].aperture *= 0.1;
    }
 
    // move lenses so last lens is at origin
    float summedThickness = 0.0;
    for(int i = 0; i < ld->lensCount; i++){
        summedThickness += ld->lenses[i].thickness;
    }
 
    ld->lenses[0].thickness -= summedThickness;
}
 
 
void computeLensCenters(Lensdata *ld){
    // precomputes the lens centers so they can just be called at every ray creation
    float summedThickness;
 
    for(int i = 0; i < ld->lensCount; i++){
        i == 0 ? summedThickness = ld->lenses[0].thickness : summedThickness += ld->lenses[i].thickness;
        ld->lenses[i].center = summedThickness - ld->lenses[i].curvature;
    }
}
 
 
inline bool raySphereIntersection(AtVector *hit_point, AtVector ray_direction, AtVector ray_origin, AtVector sphere_center, float sphere_radius, bool reverse, bool tracingRealRays){
    ray_direction = AiV3Normalize(ray_direction);
    AtVector L = sphere_center - ray_origin;
 
    float tca = AiV3Dot(L, ray_direction);
    float radius2 = SQR(sphere_radius);
    float d2 = AiV3Dot(L, L) - SQR(tca);
 
    // if the distance from the ray to the spherecenter is larger than its radius, don't worry about it
    if (tracingRealRays && (d2 > radius2)){return false;}
 
    float thc = std::sqrt(ABS(radius2 - d2));
 
    if(reverse){
        *hit_point = ray_origin + ray_direction * (tca - thc * SGN(sphere_radius));
    } else {
        *hit_point = ray_origin + ray_direction * (tca + thc * SGN(sphere_radius));
    }

    return true;
}
 
 
inline bool intersectionNormal(AtVector hit_point, AtVector sphere_center, float sphere_radius, AtVector *hit_point_normal){
    *hit_point_normal = AiV3Normalize(sphere_center - hit_point) * SGN(sphere_radius);
    return true;
}
 
 
inline bool calculateTransmissionVector(AtVector *ray_direction, float ior1, float ior2, AtVector incidentVector, AtVector normalVector, bool tracingRealRays){
    incidentVector = AiV3Normalize(incidentVector);
    normalVector = AiV3Normalize(normalVector);
 
    float eta;
    ior2 == 1.0 ? eta = ior1 : eta = ior1 / ior2;
 
    float c1 = - AiV3Dot(incidentVector, normalVector);
    float cs2 = SQR(eta) * (1.0 - SQR(c1));
 
    // total internal reflection, can only occur when ior1 > ior2
    if((tracingRealRays) && (ior1 > ior2) && (cs2 > 1.0)){
        ++ld.totalInternalReflection;
        return false;
    }
 
    *ray_direction = (incidentVector * eta) + (normalVector * ((eta * c1) - std::sqrt(ABS(1.0 - cs2))));
    return true;
}


AtVector2 lineLineIntersection(AtVector line1_origin, AtVector line1_direction, AtVector line2_origin, AtVector line2_direction){
    float A1 = line1_direction.y - line1_origin.y;
    float B1 = line1_origin.z - line1_direction.z;
    float C1 = A1 * line1_origin.z + B1 * line1_origin.y;
    float A2 = line2_direction.y - line2_origin.y;
    float B2 = line2_origin.z - line2_direction.z;
    float C2 = A2 * line2_origin.z + B2 * line2_origin.y;
    float delta = A1 * B2 - A2 * B1;
    return {(B2 * C1 - B1 * C2) / delta, (A1 * C2 - A2 * C1) / delta};
}
 
 
AtVector linePlaneIntersection(AtVector rayOrigin, AtVector rayDirection) {
    // fixed intersection with y = 0
    AtVector coord = {100.0, 0.0, 100.0};
    AtVector planeNormal = {0.0, 1.0, 0.0};
    rayDirection = AiV3Normalize(rayDirection);
    coord = AiV3Normalize(coord);

    return rayOrigin + (rayDirection * (AiV3Dot(coord, planeNormal) - AiV3Dot(planeNormal, rayOrigin)) / AiV3Dot(planeNormal, rayDirection));
}
 
 
float calculateImageDistance(float objectDistance, Lensdata *ld){
    AtVector ray_origin = {0.0f, 0.0f, objectDistance};
    AtVector ray_direction = {0.0f, (ld->lenses[ld->lensCount - 1].aperture / 2.0f) * 0.05f, - objectDistance};

    float summedThickness = 0.0, imageDistance = 0.0;
    AtVector hit_point_normal, hit_point;

    for(int k = 0; k < ld->lensCount; k++){
        summedThickness += ld->lenses[k].thickness;
    }
 
    for(int i = 0; i < ld->lensCount; i++){
        if (i != 0){summedThickness -= ld->lenses[ld->lensCount - i].thickness;}

        AtVector sphere_center = {0.0f, 0.0f, summedThickness - ld->lenses[ld->lensCount - 1 - i].curvature};

        raySphereIntersection(&hit_point, ray_direction, ray_origin, sphere_center, ld->lenses[ld->lensCount - 1 - i].curvature, true, false);
        intersectionNormal(hit_point, sphere_center, - ld->lenses[ld->lensCount - 1 - i].curvature, &hit_point_normal);

         if(i==0){
            calculateTransmissionVector(&ray_direction, 1.0, ld->lenses[ld->lensCount - i - 1].ior, ray_direction, hit_point_normal, false);
         } else {
            calculateTransmissionVector(&ray_direction, ld->lenses[ld->lensCount - i].ior, ld->lenses[ld->lensCount - i - 1].ior, ray_direction, hit_point_normal, false);
         }

         if(i == ld->lensCount - 1){
            imageDistance = linePlaneIntersection(hit_point, ray_direction).z;
         }

         ray_origin = hit_point;
     }
 
     AiMsgInfo( "%-40s %12.8f", "[ZOIC] Object distance [cm]", objectDistance);
     AiMsgInfo( "%-40s %12.8f", "[ZOIC] Image distance [cm]", imageDistance);
    
     return imageDistance;
}
 
 
inline bool traceThroughLensElements(AtVector *ray_origin, AtVector *ray_direction, Lensdata *ld, bool draw){
    AtVector hit_point, hit_point_normal, sphere_center;
 
    for(int i = 0; i < ld->lensCount; i++){
        sphere_center = {0.0, 0.0, ld->lenses[i].center};

        if(!raySphereIntersection(&hit_point, *ray_direction, *ray_origin, sphere_center, ld->lenses[i].curvature, false, true)){
            return false;
        }

        float hitPoint2 = SQR(hit_point.x) + SQR(hit_point.y);
 
        // check if ray hits lens boundary or aperture
        if ((hitPoint2 > SQR(ld->lenses[i].aperture * 0.5)) ||
            ((i == ld->apertureElement) && (hitPoint2 > SQR(ld->userApertureRadius)))){
                return false;
        }
        
        intersectionNormal(hit_point, sphere_center, ld->lenses[i].curvature, &hit_point_normal);
 
        DRAW_ONLY({
            if(draw){
                myfile << std::fixed << std::setprecision(10) << - ray_origin->z;
                myfile << " ";
                myfile << std::fixed << std::setprecision(10) << - ray_origin->y;
                myfile << " ";
                myfile << std::fixed << std::setprecision(10) << - hit_point.z;
                myfile << " ";
                myfile << std::fixed << std::setprecision(10) << - hit_point.y;
                myfile << " ";
            }})
 
        *ray_origin = hit_point;
 
        // if not last lens element
        if(i != ld->lensCount - 1){
            if(!calculateTransmissionVector(ray_direction, ld->lenses[i].ior, ld->lenses[i+1].ior, *ray_direction, hit_point_normal, true)){
                return false;
            }
        } else { // last lens element
            // assuming the material outside the lens is air [ior 1.0]
            if(!calculateTransmissionVector(ray_direction, ld->lenses[i].ior, 1.0, *ray_direction, hit_point_normal, true)){
                return false;
            }
 
            DRAW_ONLY({
                if (draw){
                    myfile << std::fixed << std::setprecision(10) << - hit_point.z;
                    myfile << " ";
                    myfile << std::fixed << std::setprecision(10) << - hit_point.y;
                    myfile << " ";
                    myfile << std::fixed << std::setprecision(10) <<  hit_point.z + ray_direction->z * -10000.0;
                    myfile << " ";
                    myfile << std::fixed << std::setprecision(10) <<  hit_point.y + ray_direction->y * -10000.0;
                    myfile << " ";
                }
            })
        }
    }
 
    return true;
}


float traceThroughLensElementsForFocalLength(Lensdata *ld, bool originShift){
    float tracedFocalLength = 0.0, focalPointDistance = 0.0, principlePlaneDistance = 0.0, summedThickness = 0.0;
    float rayOriginHeight = ld->lenses[0].aperture * 0.1;
    AtVector hit_point, hit_point_normal;
    AtVector ray_origin = {0.0, rayOriginHeight, 0.0};
    AtVector ray_direction = {0.0, 0.0, 99999.0};
 
    for(int i = 0; i < ld->lensCount; i++){
        // need to keep the summedthickness method since the sphere centers get computed only later on
        i == 0 ? summedThickness = ld->lenses[0].thickness : summedThickness += ld->lenses[i].thickness;
 
        AtVector sphere_center = {0.0, 0.0, summedThickness - ld->lenses[i].curvature};
        raySphereIntersection(&hit_point, ray_direction, ray_origin, sphere_center, ld->lenses[i].curvature, false, false);
        intersectionNormal(hit_point, sphere_center, ld->lenses[i].curvature, &hit_point_normal);
 
        if(i != ld->lensCount - 1){
            calculateTransmissionVector(&ray_direction, ld->lenses[i].ior, ld->lenses[i+1].ior, ray_direction, hit_point_normal, true);
        } else { // last element in vector
            calculateTransmissionVector(&ray_direction, ld->lenses[i].ior, 1.0, ray_direction, hit_point_normal, true);
 
            // original parallel ray start and end
            AtVector pp_line1start = {0.0, rayOriginHeight, 0.0};
            AtVector pp_line1end = {0.0, rayOriginHeight, 999999.0};
 
            // direction ray end
            AtVector pp_line2end = {0.0, static_cast<float>(ray_origin.y + (ray_direction.y * 100000.0)), static_cast<float>(ray_origin.z + (ray_direction.z * 100000.0))};
 
            principlePlaneDistance = lineLineIntersection(pp_line1start, pp_line1end, ray_origin, pp_line2end).x;
           
            if(!originShift){
                AiMsgInfo( "%-40s %12.8f", "[ZOIC] Principle Plane distance [cm]", principlePlaneDistance);
             } else {
                AiMsgInfo( "%-40s %12.8f", "[ZOIC] Adj. PP distance [cm]", principlePlaneDistance);
            }
 
            focalPointDistance = linePlaneIntersection(ray_origin, ray_direction).z;
           
            if(!originShift){
                 AiMsgInfo( "%-40s %12.8f", "[ZOIC] Focal point distance [cm]", focalPointDistance);
             } else {
                 AiMsgInfo( "%-40s %12.8f", "[ZOIC] Adj. Focal point distance [cm]", focalPointDistance);
            }
        }
 
        ray_origin = hit_point;
    }
 
    tracedFocalLength = focalPointDistance - principlePlaneDistance;
   
    if(!originShift){
        AiMsgInfo( "%-40s %12.8f", "[ZOIC] Raytraced Focal Length [cm]", tracedFocalLength);
    } else {
        AiMsgInfo( "%-40s %12.8f", "[ZOIC] Adj. Raytraced Focal Length [cm]", tracedFocalLength);
 
    }
 
    return tracedFocalLength;
}
 
 
void adjustFocalLength(Lensdata *ld){
    for(int i = 0; i < ld->lensCount; i++){
        ld->lenses[i].curvature *= ld->focalLengthRatio;
        ld->lenses[i].thickness *= ld->focalLengthRatio;
        ld->lenses[i].aperture *= ld->focalLengthRatio;
    }
}
 

void writeToFile(Lensdata *ld){
     myfile << "LENSES{";
     for(int i = 0; i < ld->lensCount; i++){
         // lenscenter, radius, angle
         myfile << std::fixed << std::setprecision(10) << -ld->lenses[i].center;
         myfile << " ";
         myfile << std::fixed << std::setprecision(10) << -ld->lenses[i].curvature;
         myfile << " ";
         myfile << std::fixed << std::setprecision(10) << (std::asin((ld->lenses[i].aperture * 0.5) / ld->lenses[i].curvature)) * (180 / AI_PI);
         myfile << " ";
     }
     myfile << "}\n";
 
     myfile << "IOR{";
     for(int i = 0; i < ld->lensCount; i++){
         myfile << std::fixed << std::setprecision(10) << ld->lenses[i].ior;
         myfile << " ";
     }
     myfile << "}\n";
 
     myfile << "APERTUREELEMENT{";
     myfile << std::fixed << std::setprecision(10) << ld->apertureElement;
     myfile << "}\n";
 
     myfile << "APERTUREDISTANCE{";
     myfile << std::fixed << std::setprecision(10) << -ld->apertureDistance;
     myfile << "}\n";
 
     myfile << "APERTURE{";
     myfile << std::fixed << std::setprecision(10) << ld->userApertureRadius;
     myfile << "}\n";
 
     myfile << "APERTUREMAX{";
     float maxAperture = 0.0;
     for(int i = 0; i < ld->lensCount; i++){
         if (ld->lenses[i].aperture > maxAperture){
             maxAperture = ld->lenses[i].aperture;
         }
     }
     myfile << std::fixed << std::setprecision(10) << maxAperture;
     myfile << "}\n";
 
     myfile << "FOCUSDISTANCE{";
     myfile << std::fixed << std::setprecision(10) << -ld->focalDistance;
     myfile << "}\n";
 
     myfile << "IMAGEDISTANCE{";
     myfile << std::fixed << std::setprecision(10) << -ld->originShift;
     myfile << "}\n";
 
     myfile << "SENSORHEIGHT{";
     myfile << std::fixed << std::setprecision(10) << 1.7;
     myfile << "}\n";
}


bool empericalOpticalVignetting(AtPoint origin, AtVector direction, float apertureRadius, float opticalVignettingRadius, float opticalVignettingDistance){
    // because the first intersection point of the aperture is already known, I can just linearly scale it by the distance to the second aperture
    AtPoint opticalVignetPoint;
    opticalVignetPoint = (direction * opticalVignettingDistance) - origin;
    float pointHypotenuse = std::sqrt(SQR(opticalVignetPoint.x) + SQR(opticalVignetPoint.y));
    float virtualApertureTrueRadius = apertureRadius * opticalVignettingRadius;

    if (ABS(pointHypotenuse) < virtualApertureTrueRadius){
        return true;
    } else {
        return false;
    }
}


inline void printProgressBar(float progress, int barWidth){
    std::cout << "\x1b[1;32m[";
    int pos = barWidth * progress;

    for (int i = 0; i < barWidth; ++i) {
        if (i < pos){std::cout << "=";}
        else if (i == pos){std::cout << ">";}
        else {std::cout << " ";}
    }

    if (progress > 1.0){progress = 1.0;}

    std::cout << std::fixed << std::setprecision(2) << "] % " << progress * 100.0 << "\r";
    std::cout.flush();
}


bool traceThroughLensElementsForApertureSize(AtVector ray_origin, AtVector ray_direction, Lensdata *ld){
    AtVector hit_point, hit_point_normal, sphere_center;
 
    for(int i = 0; i < ld->lensCount; i++){
        sphere_center = {0.0, 0.0, ld->lenses[i].center};

        if(!raySphereIntersection(&hit_point, ray_direction, ray_origin, sphere_center, ld->lenses[i].curvature, false, true)){
            return false;
        }

        float hitPoint2 = SQR(hit_point.x) + SQR(hit_point.y);
 
        // check if ray hits lens boundary or aperture
        if ((hitPoint2 > (ld->lenses[i].aperture * 0.5) * (ld->lenses[i].aperture * 0.5)) ||
            ((i == ld->apertureElement) && (hitPoint2 > SQR(ld->userApertureRadius)))){
                return false;
        }
        
        intersectionNormal(hit_point, sphere_center, ld->lenses[i].curvature, &hit_point_normal);
 
        ray_origin = hit_point;
 
        // if not last lens element
        if(i != ld->lensCount - 1){
            if(!calculateTransmissionVector(&ray_direction, ld->lenses[i].ior, ld->lenses[i+1].ior, ray_direction, hit_point_normal, true)){
                return false;
            }
        } else { // last lens element
            // assuming the material outside the lens is air [ior 1.0]
            if(!calculateTransmissionVector(&ray_direction, ld->lenses[i].ior, 1.0, ray_direction, hit_point_normal, true)){
                return false;
            }
        }
    }
 
     return true;
}


void testAperturesTruth(Lensdata *ld){
    WORK_ONLY(testAperturesFile.open ("C:/ilionData/Users/zeno.pelgrims/Documents/zoic_compile/testApertures.zoic", std::ofstream::out | std::ofstream::trunc);)
    MACBOOK_ONLY(testAperturesFile.open ("/Volumes/ZENO_2016/projects/zoic/src/testApertures.zoic", std::ofstream::out | std::ofstream::trunc);)

    AtVector origin, direction;

    int filmSamples = 3;
    int apertureSamples = 150000;

    for (int i = - filmSamples; i < filmSamples + 1; i++){
        for (int j = -filmSamples; j < filmSamples + 1; j++){
            AtPoint2 lens = {0.0, 0.0};
            testAperturesFile << "GT: ";

            for (int k = 0; k < apertureSamples; k++){
                concentricDiskSample(xor128() / 4294967296.0f, xor128() / 4294967296.0f, &lens);

                origin.x = (static_cast<float>(i) / static_cast<float>(filmSamples)) * (3.6 * 0.5);
                origin.y = (static_cast<float>(j) / static_cast<float>(filmSamples)) * (3.6 * 0.5);
                origin.z = ld->originShift;
            
                direction.x = (lens.x * ld->lenses[0].aperture) - origin.x;
                direction.y = (lens.y * ld->lenses[0].aperture) - origin.y;
                direction.z = - ld->lenses[0].thickness;

                if(traceThroughLensElements(&origin, &direction, ld, false)){
                    testAperturesFile << lens.x * ld->lenses[0].aperture << " " << lens.y * ld->lenses[0].aperture << " ";
                }
            }

            testAperturesFile << std::endl;
        }
    }

    AiMsgInfo( "%-40s", "[ZOIC] Tested Ground Truth");
}


void testAperturesLUT(Lensdata *ld){
    AtVector origin, direction;

    int filmSamples = 3;
    int apertureSamples = 15000;

    float samplingErrorCorrection = 1.15;

    for (int i = - filmSamples; i < filmSamples + 1; i++){
        for (int j = -filmSamples; j < filmSamples + 1; j++){
            
            testAperturesFile << "SS: ";

            for (int k = 0; k < apertureSamples; k++){

                origin.x = (static_cast<float>(i) / static_cast<float>(filmSamples)) * (3.6 * 0.5);
                origin.y = (static_cast<float>(j) / static_cast<float>(filmSamples)) * (3.6 * 0.5);
                origin.z = ld->originShift;

                // lowest bound x value
                std::map<float, std::map<float, boundingBox2d>>::iterator low;
                low = ld->apertureMap.lower_bound(origin.x);
                float value1 = low->first;

                // lowest bound y value
                std::map<float, boundingBox2d>::iterator low2;
                low2 = low->second.lower_bound(origin.y);
                float value2 = low2->first;

                AtPoint2 lens = {0.0, 0.0};
                concentricDiskSample(xor128() / 4294967296.0f, xor128() / 4294967296.0f, &lens);

                // go back 1 element in sorted map
                --low;
                float value3 = low->first;
                --low2;
                float value4 = low2->first;

                // percentage of x inbetween two stored LUT entries
                float xpercentage = (origin.x - value1) / (value3 - value1);
                float ypercentage = (origin.y - value2) / (value4 - value2);

                // scale
                float maxScale = BILERP(xpercentage, ypercentage, 
                                        ld->apertureMap[value1][value2].getMaxScale(), ld->apertureMap[value3][value4].getMaxScale(),
                                        ld->apertureMap[value1][value4].getMaxScale(), ld->apertureMap[value3][value2].getMaxScale()) * samplingErrorCorrection;

                lens *= {maxScale, maxScale};

                // translation
                lens += {BILERP(xpercentage, ypercentage, ld->apertureMap[value1][value2].getCentroid().x, ld->apertureMap[value3][value4].getCentroid().x, 
                                                          ld->apertureMap[value1][value4].getCentroid().x, ld->apertureMap[value3][value2].getCentroid().x),
                         BILERP(xpercentage, ypercentage, ld->apertureMap[value1][value2].getCentroid().y, ld->apertureMap[value3][value4].getCentroid().y,
                                                          ld->apertureMap[value1][value4].getCentroid().y, ld->apertureMap[value3][value2].getCentroid().y)};

                direction = {lens.x - origin.x, lens.y - origin.y, - ld->lenses[0].thickness};

                testAperturesFile << lens.x << " " << lens.y << " ";
            }

            testAperturesFile << std::endl;
        }
    }

    testAperturesFile.close();

    // execute python drawing
    WORK_ONLY(std::string filename = "C:/ilionData/Users/zeno.pelgrims/Documents/zoic/zoic/src/triangleSamplingDraw.py";)
    MACBOOK_ONLY(std::string filename = "/Volumes/ZENO_2016/projects/zoic/src/triangleSamplingDraw.py";)
    std::string command = "python "; command += filename; system(command.c_str());

    AiMsgInfo( "%-40s", "[ZOIC] Tested LUT");
}


void exitPupilLUT(Lensdata *ld, int filmSamplesX, int filmSamplesY, int boundsSamples){
    
    float progress = 0.0;
    int barWidth = 71;
    int progressPrintCounter = 0;
    
    float filmWidth = 6.0;
    float filmHeight = 6.0;

    float filmSpacingX = filmWidth / static_cast<float>(filmSamplesX);
    float filmSpacingY = filmHeight / static_cast<float>(filmSamplesY);

    AiMsgInfo( "%-40s %12d", "[ZOIC] Calculating LUT of size ^ 2", filmSamplesX);

    for(int i = 0; i < filmSamplesX + 1; i++){
        for(int j = 0; j < filmSamplesY + 1; j++){
            AtVector sampleOrigin = {static_cast<float>((filmSpacingX * static_cast<float>(i) * 2.0) - filmWidth / 2.0), 
                                     static_cast<float>((filmSpacingY * static_cast<float>(j) * 2.0) - filmHeight / 2.0), 
                                     ld->originShift};


            // calculate bounds of aperture, to find centroid
            boundingBox2d apertureBounds;
            apertureBounds.min = {0.0, 0.0};
            apertureBounds.max = {0.0, 0.0};

            AtVector boundsDirection;

            float lensU = 0.0;
            float lensV = 0.0;

            for(int b = 0; b < boundsSamples; b++){
                lensU = ((xor128() / 4294967296.0f) * 2.0f) - 1.0f;
                lensV = ((xor128() / 4294967296.0f) * 2.0f) - 1.0f;

                boundsDirection.x = (lensU * ld->lenses[0].aperture) - sampleOrigin.x;
                boundsDirection.y = (lensV * ld->lenses[0].aperture) - sampleOrigin.y;
                boundsDirection.z = - ld->lenses[0].thickness;

                if(traceThroughLensElementsForApertureSize(sampleOrigin, boundsDirection, ld)){
                    if((apertureBounds.min.x + apertureBounds.min.y) == 0.0){
                        apertureBounds.min = {lensU * ld->lenses[0].aperture, lensV * ld->lenses[0].aperture};
                        apertureBounds.max = {lensU * ld->lenses[0].aperture, lensV * ld->lenses[0].aperture};
                    }

                    if((lensU * ld->lenses[0].aperture) > apertureBounds.max.x){
                        apertureBounds.max.x = lensU * ld->lenses[0].aperture;
                    }

                    if((lensV * ld->lenses[0].aperture) > apertureBounds.max.y){
                        apertureBounds.max.y = lensV * ld->lenses[0].aperture;
                    }

                    if((lensU * ld->lenses[0].aperture) < apertureBounds.min.x){
                        apertureBounds.min.x = lensU * ld->lenses[0].aperture;
                    }

                    if((lensV * ld->lenses[0].aperture) < apertureBounds.min.y){
                        apertureBounds.min.y = lensV * ld->lenses[0].aperture;
                    }
                }
            }
            
            ld->apertureMap[sampleOrigin.x].insert(std::make_pair(sampleOrigin.y, apertureBounds));

            if (progressPrintCounter == (filmSamplesX * filmSamplesY) / 100){
                printProgressBar(progress, barWidth);
                progress = static_cast<float>((i * filmSamplesX) + j) / static_cast<float>(filmSamplesX * filmSamplesY);
                progressPrintCounter = 0; 
            } else {
                ++progressPrintCounter;
            }
        }
    }

    std::cout << "\e[0m" << std::endl;
}


node_parameters {
    AiParameterFLT("sensorWidth", 3.6); // 35mm film
    AiParameterFLT("sensorHeight", 2.4); // 35 mm film
    AiParameterFLT("focalLength", 5.0); // in cm
    AiParameterFLT("fStop", 1.4);
    AiParameterFLT("focalDistance", 50.0);
    AiParameterBOOL("useImage", true);
    AiParameterStr("bokehPath", "");
    AiParameterENUM("lensModel", THINLENS, LensModelNames);
    AiParameterStr("lensDataPath", "");
    AiParameterBOOL("kolbSamplingLUT", true);
    AiParameterBOOL("useDof", true);
    AiParameterFLT("opticalVignettingDistance", 0.0); // distance of the opticalVignetting virtual aperture
    AiParameterFLT("opticalVignettingRadius", 1.0); // 1.0 - .. range float, to multiply with the actual aperture radius
    AiParameterFLT("exposureControl", 0.0);
}
 
 
node_initialize {
     cameraData *camera = new cameraData();
     AiCameraInitialize(node, (void*)camera);
 
     DRAW_ONLY(AiMsgInfo("[ZOIC] ---- IMAGE DRAWING ENABLED @ COMPILE TIME ----");)
}
 
 
node_update {
    AiCameraUpdate(node, false);
    cameraData *camera = (cameraData*) AiCameraGetLocalData(node);
    AtNode* options = AiUniverseGetOptions();
 
    DRAW_ONLY({
        // create file to transfer data to python drawing module
        MACBOOK_ONLY(myfile.open("/Volumes/ZENO_2016/projects/zoic/src/draw.zoic");)
        WORK_ONLY(myfile.open("C:/ilionData/Users/zeno.pelgrims/Documents/zoic_compile/draw.zoic", std::ofstream::out | std::ofstream::trunc);)
    })
 
    // make probability functions of the bokeh image
    if (params[p_useImage].BOOL == true){
        if (!camera->image.read(params[p_bokehPath].STR)){
            AiMsgError("[ZOIC] Couldn't open bokeh image!");
            AiRenderAbort();
        }
    }
 
    camera->image.invalidate();
 

    switch(params[p_lensModel].INT)
    {
    case THINLENS:
        {
            DRAW_ONLY({
                myfile << "LENSMODEL{THINLENS}";
                myfile << "\n";
                myfile << "RAYS{" ;
            })

            camera->fov = 2.0f * atan((params[p_sensorWidth].FLT / (2.0f * params[p_focalLength].FLT))); // in radians
            camera->tan_fov = tanf(camera->fov/ 2.0f);
            camera->apertureRadius = (params[p_focalLength].FLT) / (2.0f * params[p_fStop].FLT);
        }
        break;

    case RAYTRACED:
        {
            // check if i actually need to recalculate everything, or parameters didn't change on update
            if(!(ldCheckUpdate.stored_sensorWidth == params[p_sensorWidth].FLT && ldCheckUpdate.stored_sensorHeight == params[p_sensorHeight].FLT  &&
                ldCheckUpdate.stored_focalLength == params[p_focalLength].FLT && ldCheckUpdate.stored_fStop == params[p_fStop].FLT &&
                ldCheckUpdate.stored_focalDistance == params[p_focalDistance].FLT && ldCheckUpdate.stored_useImage == params[p_useImage].BOOL &&
                ldCheckUpdate.stored_bokehPath == params[p_bokehPath].STR && ldCheckUpdate.stored_lensDataPath == params[p_lensDataPath].STR &&
                ldCheckUpdate.stored_kolbSamplingLUT == params[p_kolbSamplingLUT].BOOL)){

                // update everything
                ldCheckUpdate.stored_sensorWidth = params[p_sensorWidth].FLT;
                ldCheckUpdate.stored_sensorHeight = params[p_sensorHeight].FLT;
                ldCheckUpdate.stored_focalLength = params[p_focalLength].FLT;
                ldCheckUpdate.stored_fStop = params[p_fStop].FLT;
                ldCheckUpdate.stored_focalDistance = params[p_focalDistance].FLT;
                ldCheckUpdate.stored_useImage = params[p_useImage].BOOL;
                ldCheckUpdate.stored_bokehPath = params[p_bokehPath].STR;
                ldCheckUpdate.stored_lensDataPath = params[p_lensDataPath].STR;
                ldCheckUpdate.stored_kolbSamplingLUT = params[p_kolbSamplingLUT].BOOL;

                DRAW_ONLY({
                     myfile << "LENSMODEL{KOLB}";
                     myfile << "\n";
                 })
         
                // reset variables
                ld.lenses.clear();
                ld.vignettedRays = 0;
                ld.succesRays = 0;
                ld.totalInternalReflection = 0;
                ld.originShift = 0.0;
                ld.apertureMap.clear();
         
                // not sure if this is the right way to do it.. probably more to it than this!
                ld.filmDiagonal = std::sqrt(SQR(params[p_sensorWidth].FLT) + SQR(params[p_sensorHeight].FLT));
                
                ld.focalDistance = params[p_focalDistance].FLT;
         
                // check if file is supplied
                // string is const char* so have to do it the oldskool way
                if ((params[p_lensDataPath].STR != NULL) && (params[p_lensDataPath].STR[0] == '\0')){
                   AiMsgError("[ZOIC] Lens Data Path is empty");
                   AiRenderAbort();
                } else {
                   AiMsgInfo("[ZOIC] Lens Data Path = [%s]", params[p_lensDataPath].STR);
                   readTabularLensData(params[p_lensDataPath].STR, &ld);
                }
         
                // look for invalid numbers that would mess it all up bro
                cleanupLensData(&ld);
         
                // calculate focal length by tracing a parallel ray through the lens system
                float kolbFocalLength = traceThroughLensElementsForFocalLength(&ld, false);
         
                // find by how much all lens elements should be scaled
                ld.focalLengthRatio = params[p_focalLength].FLT / kolbFocalLength;
                AiMsgInfo( "%-40s %12.8f", "[ZOIC] Focal length ratio", ld.focalLengthRatio);
         
                // scale lens elements
                adjustFocalLength(&ld);
         
                // calculate focal length by tracing a parallel ray through the lens system (2nd time for new focallength)
                kolbFocalLength = traceThroughLensElementsForFocalLength(&ld, true);
         
                // user specified aperture radius from fstop
                ld.userApertureRadius = kolbFocalLength / (2.0 * params[p_fStop].FLT);
                AiMsgInfo( "%-40s %12.8f", "[ZOIC] User aperture radius [cm]", ld.userApertureRadius);
         
                // clamp aperture if fstop is wider than max aperture given by lens description
                if (ld.userApertureRadius > ld.lenses[ld.apertureElement].aperture){
                    AiMsgWarning("[ZOIC] Given FSTOP wider than maximum aperture radius provided by lens data.");
                    AiMsgWarning("[ZOIC] Clamping aperture radius from [%.9f] to [%.9f]", ld.userApertureRadius, ld.lenses[ld.apertureElement].aperture);
                    ld.userApertureRadius = ld.lenses[ld.apertureElement].aperture;
                }
               
                // calculate how much origin should be shifted so that the image distance at a certain object distance falls on the film plane
                ld.originShift = calculateImageDistance(params[p_focalDistance].FLT, &ld);
         
                // calculate distance between film plane and aperture
                ld.apertureDistance = 0.0;
                for(int i = 0; i < ld.lensCount; i++){
                    ld.apertureDistance += ld.lenses[i].thickness;
                    if(i == ld.apertureElement){
                        AiMsgInfo( "%-40s %12.8f", "[ZOIC] Aperture distance [cm]", ld.apertureDistance);
                        break;
                    }
                }
         
                // precompute lens centers
                computeLensCenters(&ld);

                // precompute aperture lookup table
                if (params[p_kolbSamplingLUT].BOOL){
                    exitPupilLUT(&ld, 64, 64, 25000);

                    DRAW_ONLY({
                        //testAperturesTruth(&ld);
                        //testAperturesLUT(&ld);
                    }) 
                }
                     
                DRAW_ONLY({
                    // write to file for lens drawing
                    writeToFile(&ld);
                    myfile << "RAYS{";
                })

            } else {
                AiMsgWarning("[ZOIC] Skipping raytraed node update, parameters didn't change.");
            }
        }

        break;
    }
 
}
 

node_finish {
    cameraData *camera = (cameraData*) AiCameraGetLocalData(node);
 
    AiMsgInfo( "%-40s %12d", "[ZOIC] Succesful rays", ld.succesRays);
    AiMsgInfo( "%-40s %12d", "[ZOIC] Vignetted rays", ld.vignettedRays);
    AiMsgInfo( "%-40s %12.8f", "[ZOIC] Vignetted Percentage", (static_cast<float>(ld.vignettedRays) / (static_cast<float>(ld.succesRays) + static_cast<float>(ld.vignettedRays))) * 100.0);
    AiMsgInfo( "%-40s %12d", "[ZOIC] Total internal reflection cases", ld.totalInternalReflection);
   
    DRAW_ONLY({
        AiMsgInfo( "%-40s %12d", "[ZOIC] Rays to be drawn", ld.drawRays);
 
        myfile << "}";
        myfile.close();
 
        // execute python drawing
        MACBOOK_ONLY(std::string filename = "/Volumes/ZENO_2016/projects/zoic/src/draw.py";)
        WORK_ONLY(std::string filename = "C:/ilionData/Users/zeno.pelgrims/Documents/zoic_compile/draw.py";)
        std::string command = "python "; command += filename; system(command.c_str());
 
        AiMsgInfo("[ZOIC] Drawing finished");
    })
 
    delete camera;
    AiCameraDestroy(node);
}


camera_create_ray {
    const AtParamValue* params = AiNodeGetParams(node);
    cameraData *camera = (cameraData*) AiCameraGetLocalData(node);

    DRAW_ONLY({
        // tmp draw counters
        if (counter == 100000){
            draw = true;
            counter = 0;
        }
    })

    int tries = 0;
    int maxtries = 15;

    switch(params[p_lensModel].INT)
    {
    case THINLENS:
        {
            // create point on lens
            AtPoint p = {input->sx * camera->tan_fov, input->sy * camera->tan_fov, 1.0};
            output->dir = AiV3Normalize(p - output->origin);

            AtPoint originOriginal = output->origin;
     
            // DOF CALCULATIONS
            if (params[p_useDof].BOOL == true) {

                AtPoint2 lens = {0.0f, 0.0f};
                if(!params[p_useImage].BOOL){
                    concentricDiskSample(input->lensx, input->lensy, &lens);
                } else {
                    camera->image.bokehSample(input->lensx, input->lensy, &lens.x, &lens.y);
                }

                lens *= camera->apertureRadius;
                output->origin = {lens.x, lens.y, 0.0};
     
                // Compute point on plane of focus, intersection on z axis
                float intersection = ABS(params[p_focalDistance].FLT / output->dir.z);
                AtPoint focusPoint = output->dir * intersection;
                output->dir = AiV3Normalize(focusPoint - output->origin);

                if (params[p_opticalVignettingDistance].FLT > 0.0f){
                    while(!empericalOpticalVignetting(output->origin, output->dir, camera->apertureRadius, params[p_opticalVignettingRadius].FLT, params[p_opticalVignettingDistance].FLT) && tries <= maxtries){
                        
                        if (!params[p_useImage].BOOL){
                            concentricDiskSample(xor128() / 4294967296.0f, xor128() / 4294967296.0f, &lens);
                        } else {
                            camera->image.bokehSample(xor128() / 4294967296.0f, xor128() / 4294967296.0f, &lens.x, &lens.y);
                        }

                        lens *= camera->apertureRadius;
                        output->dir = AiV3Normalize(p - originOriginal);
                        output->origin = {lens.x, lens.y, 0.0};
                        float intersection = ABS(params[p_focalDistance].FLT / output->dir.z);
                        AtPoint focusPoint = output->dir * intersection;
                        output->dir = AiV3Normalize(focusPoint - output->origin);
                        
                        ++tries;
                    }
                }

                if (tries > maxtries){
                    output->weight = 0.0f;
                    ++ld.vignettedRays;
                } else {
                    ++ld.succesRays;
                }
            }
     
            DRAW_ONLY({
                if (draw){
                    myfile << std::fixed << std::setprecision(10) << output->origin.z;
                    myfile << " ";
                    myfile << std::fixed << std::setprecision(10) << output->origin.y;
                    myfile << " ";
                    myfile << std::fixed << std::setprecision(10) << output->dir.z * -10000.0;
                    myfile << " ";
                    myfile << std::fixed << std::setprecision(10) << output->dir.y * 10000.0;
                    myfile << " ";
                }
     
                draw = false;
            })
     
            // now looking down -Z
            output->dir.z *= -1.0;
        }

        break;

    case RAYTRACED:
        {
            // not sure if this is correct, i´d like to use the diagonal since that seems to be the standard
            output->origin.x = input->sx * (params[p_sensorWidth].FLT * 0.5);
            output->origin.y = input->sy * (params[p_sensorWidth].FLT * 0.5);
            output->origin.z = ld.originShift;

            AtPoint kolb_origin_original = output->origin;
            
            DRAW_ONLY({
                // looks cleaner in 2d when rays are aligned on axis
                output->origin.x = 0.0;
                //output->origin.y = 0.0;
            })

            // sample disk with proper sample distribution
            AtPoint2 lens = {0.0, 0.0};
            if(!params[p_useImage].BOOL){
                concentricDiskSample(input->lensx, input->lensy, &lens);
            } else {
                camera->image.bokehSample(input->lensx, input->lensy, &lens.x, &lens.y);
            }
            
            if (!params[p_kolbSamplingLUT].BOOL){ // NAIVE OVER WHOLE FIRST LENS ELEMENT, VERY SLOW FOR SMALL APERTURES
                output->origin = kolb_origin_original;
                output->dir = {(lens.x * ld.lenses[0].aperture) - output->origin.x, (lens.y * ld.lenses[0].aperture) - output->origin.y, -ld.lenses[0].thickness};
                DRAW_ONLY(output->dir.x = 0.0;)
                while(!traceThroughLensElements(&output->origin, &output->dir, &ld, draw) && tries <= maxtries){
                    output->origin = kolb_origin_original;
                    if(!params[p_useImage].BOOL){
                        concentricDiskSample(xor128() / 4294967296.0, xor128() / 4294967296.0, &lens);
                    } else {
                        camera->image.bokehSample(xor128() / 4294967296.0, xor128() / 4294967296.0, &lens.x, &lens.y);
                    }
                    output->dir = {(lens.x * ld.lenses[0].aperture) - output->origin.x, (lens.y * ld.lenses[0].aperture) - output->origin.y, - ld.lenses[0].thickness};
                    ++tries;
                }
            } 

            else { // USING LOOKUP TABLE
                
                float samplingErrorCorrection = 1.5;

                // lowest bound x value
                std::map<float, std::map<float, boundingBox2d>>::iterator low;
                low = ld.apertureMap.lower_bound(output->origin.x);
                float value1 = low->first;

                // lowest bound y value
                std::map<float, boundingBox2d>::iterator low2;
                low2 = low->second.lower_bound(output->origin.y);
                float value2 = low2->first;

                // go back 1 element in sorted map
                --low;
                float value3 = low->first;
                --low2;
                float value4 = low2->first;

                // percentage of x inbetween two stored LUT entries
                float xpercentage = (output->origin.x - value1) / (value3 - value1);
                float ypercentage = (output->origin.y - value2) / (value4 - value2);

                // scale
                float maxScale = BILERP(xpercentage, ypercentage, ld.apertureMap[value1][value2].getMaxScale(), ld.apertureMap[value3][value4].getMaxScale(),
                                        ld.apertureMap[value1][value4].getMaxScale(), ld.apertureMap[value3][value2].getMaxScale()) * samplingErrorCorrection;

                lens *= maxScale;

                // translation
                AtPoint2 centroid1 = ld.apertureMap[value1][value2].getCentroid();
                AtPoint2 centroid2 = ld.apertureMap[value1][value4].getCentroid();
                AtPoint2 centroid3 = ld.apertureMap[value3][value4].getCentroid();
                AtPoint2 centroid4 = ld.apertureMap[value3][value2].getCentroid();

                AtPoint2 translation = {BILERP(xpercentage, ypercentage, centroid1.x, centroid3.x, centroid2.x, centroid4.x),
                                        BILERP(xpercentage, ypercentage, centroid1.y, centroid3.y, centroid2.y, centroid4.y)};

                lens += translation;
                output->dir = {lens.x - output->origin.x, lens.y - output->origin.y, - ld.lenses[0].thickness};

                
                while(!traceThroughLensElements(&output->origin, &output->dir, &ld, draw) && tries <= maxtries){
                    // reset origin and update direction with new lens coords
                    output->origin = kolb_origin_original;

                    if(!params[p_useImage].BOOL){
                        concentricDiskSample(xor128() / 4294967296.0, xor128() / 4294967296.0, &lens);
                    } else {
                        camera->image.bokehSample(xor128() / 4294967296.0, xor128() / 4294967296.0, &lens.x, &lens.y);
                    }

                    lens *= maxScale;
                    lens += translation;
                    output->dir = {lens.x - output->origin.x, lens.y - output->origin.y, - ld.lenses[0].thickness};
                    ++tries;
                }
            }

            if(tries > maxtries){
                output->weight = 0.0f;
                ++ld.vignettedRays;
            } else {
                ++ld.succesRays;
            }


            // looks cleaner in 2d when rays are aligned on axis
            DRAW_ONLY(output->dir.x = 0.0;)

            // flip ray direction and origin
            output->dir *= -1.0;
            output->origin *= -1.0;
     
            DRAW_ONLY(draw = false;)
        }

        break;
    }

 
    // control to go light stops up and down
    float e2 = SQR(params[p_exposureControl].FLT);
    if (params[p_exposureControl].FLT > 0.0f){
        output->weight *= 1.0f + e2;
    } else if (params[p_exposureControl].FLT < 0.0f){
        output->weight *= 1.0f / (1.0f + e2);
    }
 
    DRAW_ONLY(++counter;)

    // EXPERIMENTAL, I KNOW IT IS INCORRECT BUT AT LEAST THE VISUAL PROBLEM IS RESOLVED
    output->dOdy = output->origin;
    output->dDdy = output->dir;
}
 
 
 
node_loader {
  if (i > 0){return false;}
  node->methods      = zoicMethods;
  node->output_type  = AI_TYPE_NONE;
  node->name         = "zoic";
  node->node_type    = AI_NODE_CAMERA;
  strcpy(node->version, AI_VERSION);
  return true;
}