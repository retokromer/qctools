/*  Copyright (c) BAVC. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license that can
 *  be found in the License.html file in the root of the source tree.
 */

//---------------------------------------------------------------------------
#include "Core/FileInformation.h"
#include "Core/SignalServer.h"
#include "Core/FFmpeg_Glue.h"
#include "Core/BlackmagicDeckLink_Glue.h"

#include "Core/VideoStats.h"
#include "Core/AudioStats.h"
#include "Core/FormatStats.h"
#include "Core/StreamsStats.h"

#include <QProcess>
#include <QFileInfo>
#include <QDebug>
#include <QTemporaryFile>
#include <QXmlStreamWriter>
#include <QUrl>
#include <zlib.h>
#include <zconf.h>

#include <string>
#include <sstream>
#include <cassert>
#ifdef _WIN32
    #include <algorithm>
#endif

//***************************************************************************
// Simultaneous parsing
//***************************************************************************
static int ActiveParsing_Count=0;

void FileInformation::run()
{
    if(m_jobType == FileInformation::Parsing)
    {
        runParse();
    }
    else if(m_jobType == FileInformation::Exporting)
    {
        runExport();
    }
}

void FileInformation::runParse()
{
    if(signalServer->enabled())
    {
        QString statsFileName = fileName() + ".qctools.xml.gz";
        QFileInfo fileInfo(statsFileName);

        checkFileUploaded(fileInfo.fileName());
    }

#ifdef BLACKMAGICDECKLINK_YES
    if (blackmagicDeckLink_Glue)
    {
        blackmagicDeckLink_Glue->Glue=Glue;

        bool completed = false;
        for (;;)
        {
            switch (blackmagicDeckLink_Glue->Config_Out.Status)
            {
                case BlackmagicDeckLink_Glue::instancied:
                                                        blackmagicDeckLink_Glue->Start();
                                                        break;
                case BlackmagicDeckLink_Glue::finished:
                                                        completed=true;
                                                        break;
                default : ;
            }

            if (WantToStop || completed)
                break;
            yieldCurrentThread();
        }

        delete blackmagicDeckLink_Glue; blackmagicDeckLink_Glue=NULL;
    }
    else
#endif // BLACKMAGICDECKLINK_YES
    {
        int frameNumber = 1;

        for (;;)
        {
            if (Glue)
            {
                if (!Glue->NextFrame())
                    break;

                ++frameNumber;
            }
            if (WantToStop)
                break;
            yieldCurrentThread();
        }
    }

    ActiveParsing_Count--;
    m_parsed = !WantToStop;

    Q_EMIT parsingCompleted(WantToStop == false);
}

void FileInformation::runExport()
{
    Export_XmlGz(m_exportFileName);
}

//***************************************************************************
// Constructor/Destructor
//***************************************************************************

//---------------------------------------------------------------------------
FileInformation::FileInformation (SignalServer* signalServer, const QString &FileName_, activefilters ActiveFilters_, activealltracks ActiveAllTracks_,
#ifdef BLACKMAGICDECKLINK_YES
                                  BlackmagicDeckLink_Glue* blackmagicDeckLink_Glue_,
#endif // BLACKMAGICDECKLINK_YES
                                  int FrameCount, const string &Encoding_FileName, const std::string &Encoding_Format) :
    FileName(FileName_),
    ActiveFilters(ActiveFilters_),
    ActiveAllTracks(ActiveAllTracks_),
    signalServer(signalServer),
#ifdef BLACKMAGICDECKLINK_YES
    blackmagicDeckLink_Glue(blackmagicDeckLink_Glue_),
#endif // BLACKMAGICDECKLINK_YES
    m_jobType(Parsing),
	streamsStats(NULL),
    formatStats(NULL)
{
    static struct RegisterMetatypes {
        RegisterMetatypes() {
            qRegisterMetaType<SharedFile>("SharedFile");
        }
    } registerMetatypes;

    connect(this, SIGNAL(parsingCompleted(bool)), this, SLOT(parsingDone(bool)));

    QString StatsFromExternalData_FileName;
    bool StatsFromExternalData_FileName_IsCompressed=false;

    // Finding the right file names (both media file and stats file)
    if (FileName.indexOf(".qctools.xml.gz")==FileName.length()-15)
    {
        StatsFromExternalData_FileName=FileName;
        FileName.resize(FileName.length()-15);
        StatsFromExternalData_FileName_IsCompressed=true;
    }
    else if (FileName.indexOf(".qctools.xml")==FileName.length()-12)
    {
        StatsFromExternalData_FileName=FileName;
        FileName.resize(FileName.length()-12);
    }
    else if (FileName.indexOf(".xml.gz")==FileName.length()-7)
    {
        StatsFromExternalData_FileName=FileName;
        FileName.resize(FileName.length()-7);
        StatsFromExternalData_FileName_IsCompressed=true;
    }
    else if (FileName.indexOf(".xml")==FileName.length()-4)
    {
        StatsFromExternalData_FileName=FileName;
        FileName.resize(FileName.length()-4);
    }
    if (StatsFromExternalData_FileName.size()==0)
    {
        if (QFile::exists(FileName+".qctools.xml.gz"))
        {
            StatsFromExternalData_FileName=FileName+".qctools.xml.gz";
            StatsFromExternalData_FileName_IsCompressed=true;
        }
        else if (QFile::exists(FileName+".qctools.xml"))
        {
            StatsFromExternalData_FileName=FileName+".qctools.xml";
        }
        else if (QFile::exists(FileName+".xml.gz"))
        {
            StatsFromExternalData_FileName=FileName+".xml.gz";
            StatsFromExternalData_FileName_IsCompressed=true;
        }
        else if (QFile::exists(FileName+".xml"))
        {
            StatsFromExternalData_FileName=FileName+".xml";
        }
    }

    // External data optional input
    QFile StatsFromExternalData_File(StatsFromExternalData_FileName);
    bool StatsFromExternalData_IsOpen=StatsFromExternalData_File.open(QIODevice::ReadOnly);

    // Running FFmpeg
    string FileName_string=FileName.toUtf8().data();
    #ifdef _WIN32
        replace(FileName_string.begin(), FileName_string.end(), '/', '\\' );
    #endif
    string Filters[Type_Max];
    if (!StatsFromExternalData_IsOpen)
    {
        if (ActiveFilters[ActiveFilter_Video_signalstats])
            Filters[0]+=",signalstats=stat=tout+vrep+brng";
        if (ActiveFilters[ActiveFilter_Video_cropdetect])
            Filters[0]+=",cropdetect=reset=1:round=1";
        if (ActiveFilters[ActiveFilter_Video_Idet])
            Filters[0]+=",idet=half_life=1";
        if (ActiveFilters[ActiveFilter_Video_Psnr] && ActiveFilters[ActiveFilter_Video_Ssim])
        {
            Filters[0]+=",split[a][b];[a]field=top[a1];[b]field=bottom,split[b1][b2];[a1][b1]psnr[c1];[c1][b2]ssim";
        }
        else
        {
            if (ActiveFilters[ActiveFilter_Video_Psnr])
                Filters[0]+=",split[a][b];[a]field=top[a1];[b]field=bottom[b1];[a1][b1]psnr";
            if (ActiveFilters[ActiveFilter_Video_Ssim])
                Filters[0]+=",split[a][b];[a]field=top[a1];[b]field=bottom[b1];[a1][b1]ssim";
        }
        Filters[0].erase(0, 1); // remove first comma
        if (ActiveFilters[ActiveFilter_Audio_astats])
            Filters[1]+=",astats=metadata=1:reset=1:length=0.4";
        if (ActiveFilters[ActiveFilter_Audio_aphasemeter])
            Filters[1]+=",aphasemeter=video=0";
        if (ActiveFilters[ActiveFilter_Audio_EbuR128])
            Filters[1]+=",ebur128=metadata=1";
        Filters[1].erase(0, 1); // remove first comma
    }
    else
    {
        streamsStats = new StreamsStats();
        formatStats = new FormatStats();

        //Stats init
        VideoStats* Video=new VideoStats();
        Stats.push_back(Video);
        AudioStats* Audio=new AudioStats();
        Stats.push_back(Audio);

        //XML init
        const char*  Xml_HeaderFooter="</frames></ffprobe:ffprobe><ffprobe:ffprobe><frames>";
        const size_t Xml_HeaderSize=25;
        const size_t Xml_FooterSize=27;
        const size_t Xml_MaxSize=0x1000000; //Blocks of 16 MiB, arbitrary chosen
        char* Xml=new char[Xml_MaxSize+Xml_HeaderSize+Xml_FooterSize];

        //Read init
        const size_t Compressed_MaxSize=0x100000; //Blocks of 1 MiB, arbitrary chosen
        char* Compressed=new char[Compressed_MaxSize];

        //Uncompress init
        z_stream strm;
        if (StatsFromExternalData_FileName_IsCompressed)
        {
            strm.next_in = NULL;
            strm.avail_in = 0;
            strm.next_out = NULL;
            strm.avail_out = 0;
            strm.total_out = 0;
            strm.zalloc = Z_NULL;
            strm.zfree = Z_NULL;
            inflateInit2(&strm, 15 + 16); // 15 + 16 are magic values for gzip
        }

        //Load and parse compressed data chunk by chunk
        char* Xml_Pointer=Xml;
        int inflate_Result;
        int TEMP = 0;

        for (;;)
        {
            //Load
            qint64 ReadSize;
            size_t avail_out=Xml_MaxSize-(Xml_Pointer-Xml);
            if (StatsFromExternalData_FileName_IsCompressed)
                ReadSize=StatsFromExternalData_File.read(Compressed, Compressed_MaxSize); //Load in an intermediate buffer for decompression
            else
                ReadSize=StatsFromExternalData_File.read(Xml_Pointer, avail_out); //Load directly in the XML buffer
            if (!ReadSize)
                break;
            TEMP += ReadSize;

            //Parse compressed data, with handling of the case the output buffer is not big enough
            strm.next_in=(Bytef*)Compressed;
            strm.avail_in=ReadSize;
            for (;;)
            {
                size_t Xml_Size=Xml_Pointer-Xml;
                if (StatsFromExternalData_FileName_IsCompressed)
                {
                    //inflate
                    strm.next_out=(Bytef*)Xml_Pointer;
                    strm.avail_out=avail_out;
                    if ((inflate_Result=inflate(&strm, Z_NO_FLUSH))<0)
                        break;
                    Xml_Size+=avail_out-strm.avail_out;
                }
                else
                    Xml_Size+=ReadSize;

                //Cut the XML after the last XML frame footer, and keep the remaining data for the next turn
                size_t Xml_SizeForParsing=Xml_Size;
                //Look for XML frame footer
                while (Xml_SizeForParsing>Xml_HeaderSize &&
                        ( Xml[Xml_SizeForParsing-1]!='>' //"</frame>"
                    || Xml[Xml_SizeForParsing-2]!='e'
                    || Xml[Xml_SizeForParsing-3]!='m'
                    || Xml[Xml_SizeForParsing-4]!='a'
                    || Xml[Xml_SizeForParsing-5]!='r'
                    || Xml[Xml_SizeForParsing-6]!='f'
                    || Xml[Xml_SizeForParsing-7]!='/'
                    || Xml[Xml_SizeForParsing-8]!='<'))
                    Xml_SizeForParsing--;
                if (Xml_SizeForParsing<=Xml_HeaderSize)
                {
                    //The block does not contain any complete frame, looping in order to get more data from the file
                    Xml_Pointer=Xml+Xml_Size;
                    break;
                }

                //Insert Xml_HeaderFooter inside the XML content
                memmove(Xml+Xml_SizeForParsing+Xml_HeaderSize+Xml_FooterSize, Xml+Xml_SizeForParsing, Xml_Size-Xml_SizeForParsing);
                memcpy(Xml+Xml_SizeForParsing, Xml_HeaderFooter, Xml_HeaderSize+Xml_FooterSize);
                Xml_Size+=Xml_HeaderSize+Xml_FooterSize;
                Xml_SizeForParsing+=Xml_FooterSize;

                Video->StatsFromExternalData(Xml, Xml_SizeForParsing);
                Audio->StatsFromExternalData(Xml, Xml_SizeForParsing);

                //Cut the parsed content
                memmove(Xml, Xml+Xml_SizeForParsing, Xml_Size-Xml_SizeForParsing);
                Xml_Pointer=Xml+Xml_Size-Xml_SizeForParsing;

                //Check if we need to stop
                if (!StatsFromExternalData_FileName_IsCompressed || strm.avail_out || inflate_Result == Z_STREAM_END)
                    break;
            }

            //Coherency checks
            if (Xml_Pointer>=Xml+Xml_MaxSize+Xml_HeaderSize)
                break;
        }

        //Inform the parser that parsing is finished
        Video->StatsFromExternalData_Finish();
        Audio->StatsFromExternalData_Finish();

        //Parse formats
        formatStats->readFromXML(Xml, Xml_Pointer - Xml);

        //Parse streams
        streamsStats->readFromXML(Xml, Xml_Pointer - Xml);

        //Cleanup
        if (StatsFromExternalData_FileName_IsCompressed)
            inflateEnd(&strm);
        delete[] Compressed;
        delete[] Xml;

        if(signalServer->enabled())
        {
            QFileInfo fileInfo(StatsFromExternalData_FileName);
            checkFileUploaded(fileInfo.fileName());
        }
    }
    Glue=new FFmpeg_Glue(FileName_string.c_str(), ActiveAllTracks, &Stats, &streamsStats, &formatStats, Stats.empty());
    if (FileName_string.empty())
    {
#ifdef BLACKMAGICDECKLINK_YES
        Glue->AddInput_Video(FrameCount, 1001, 30000, 720, 486, blackmagicDeckLink_Glue->Config_In.VideoBitDepth, blackmagicDeckLink_Glue->Config_In.VideoCompression, blackmagicDeckLink_Glue->Config_In.TC_in);
        Glue->AddInput_Audio(FrameCount, 1001, 30000, 48000, blackmagicDeckLink_Glue->Config_In.AudioBitDepth, blackmagicDeckLink_Glue->Config_In.AudioTargetBitDepth, blackmagicDeckLink_Glue->Config_In.ChannelsCount);
#endif // BLACKMAGICDECKLINK_YES
    }
    else if (Glue->ContainerFormat_Get().empty())
    {
        delete Glue;
        Glue=NULL;
        for (size_t Pos=0; Pos<Stats.size(); Pos++)
            Stats[Pos]->StatsFinish();
    }

    if (Glue)
    {
        Glue->AddOutput(0, 72, 72, FFmpeg_Glue::Output_Jpeg);
        if (!Encoding_FileName.empty())
        {
            Glue->AddOutput(Encoding_FileName, Encoding_Format);
            FileName=QString::fromUtf8(Encoding_FileName.c_str());
        }
        Glue->AddOutput(1, 0, 0, FFmpeg_Glue::Output_Stats, 0, Filters[0]);
        Glue->AddOutput(0, 0, 0, FFmpeg_Glue::Output_Stats, 1, Filters[1]);
    }

    // Looking for the first video stream
    ReferenceStream_Pos=0;
    for (; ReferenceStream_Pos<Stats.size(); ReferenceStream_Pos++)
        if (Stats[ReferenceStream_Pos] && Stats[ReferenceStream_Pos]->Type_Get()==0)
            break;
    if (ReferenceStream_Pos>=Stats.size())
        ReferenceStream_Pos=0;

    Frames_Pos=0;
    WantToStop=false;
    startParse();
}

//---------------------------------------------------------------------------
FileInformation::~FileInformation ()
{
    WantToStop=true;
    bool result = wait();
    assert(result);

	delete streamsStats;
    delete formatStats;

    for (size_t Pos=0; Pos<Stats.size(); Pos++)
        delete Stats[Pos];
    delete Glue;
}

//***************************************************************************
// Parsing
//***************************************************************************

//---------------------------------------------------------------------------
void FileInformation::startParse ()
{
    m_jobType = Parsing;

    int Max=QThread::idealThreadCount();
    if (Max>2)
        Max-=2;
    else
        Max=1;
    if (!isRunning() && ActiveParsing_Count<Max)
    {
        if(Glue)
        {
            start();
            ActiveParsing_Count++;
        }
    }
}

void FileInformation::startExport(const QString &exportFileName)
{
    m_jobType = Exporting;
    m_exportFileName = exportFileName;

    if (!isRunning())
    {
        if(Glue)
        {
            start();
        }
    }
}

//***************************************************************************
// Actions
//***************************************************************************

//---------------------------------------------------------------------------
void FileInformation::Export_XmlGz (const QString &ExportFileName)
{
    if (!Glue)
        return;

    stringstream Data;

    // Header
    Data<<"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    Data<<"<!-- Created by QCTools " << Version << " -->\n";
    Data<<"<ffprobe:ffprobe xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance' xmlns:ffprobe='http://www.ffmpeg.org/schema/ffprobe' xsi:schemaLocation='http://www.ffmpeg.org/schema/ffprobe ffprobe.xsd'>\n";
    Data<<"    <program_version version=\"" << Glue->FFmpeg_Version() << "\" copyright=\"Copyright (c) 2007-" << Glue->FFmpeg_Year() << " the FFmpeg developers\" build_date=\"" __DATE__ "\" build_time=\"" __TIME__ "\" compiler_ident=\"" << Glue->FFmpeg_Compiler() << "\" configuration=\"" << Glue->FFmpeg_Configuration() << "\"/>\n";
    Data<<"\n";
    Data<<"    <library_versions>\n";
    Data<<Glue->FFmpeg_LibsVersion();
    Data<<"    </library_versions>\n";

    Data<<"    <frames>\n";

    // From stats
    for (size_t Pos=0; Pos<Stats.size(); Pos++)
        if (Stats[Pos])
            Data<<Stats[Pos]->StatsToXML(Glue->Width_Get(), Glue->Height_Get());

    // Footer
    Data<<"    </frames>";

    QString streamsAndFormats;
    QXmlStreamWriter writer(&streamsAndFormats);
    writer.setAutoFormatting(true);
    writer.setAutoFormattingIndent(4);

    if(streamsStats)
        streamsStats->writeToXML(&writer);

    if(formatStats)
        formatStats->writeToXML(&writer);

    // add indentation
    QStringList splitted = streamsAndFormats.split("\n");
    for(size_t i = 0; i < splitted.length(); ++i)
        splitted[i] = QString(qAbs(writer.autoFormattingIndent()), writer.autoFormattingIndent() > 0 ? ' ' : '\t') + splitted[i];
    streamsAndFormats = splitted.join("\n");

    Data<<streamsAndFormats.toStdString() << "\n\n";

    Data<<"</ffprobe:ffprobe>";

    SharedFile file;
    QString name;

    if(ExportFileName.isEmpty())
    {
        file = SharedFile(new QTemporaryFile());
        QFileInfo info(fileName() + ".qctools.xml.gz");
        name = info.fileName();
    } else {
        file = SharedFile(new QFile(ExportFileName));
        QFileInfo info(ExportFileName);
        name = info.fileName();
    }

    if (file->open(QIODevice::ReadWrite))
    {
        string DataS=Data.str();
        uLongf Buffer_Size=65536;
        char* Buffer=new char[Buffer_Size];
        z_stream strm;
        strm.next_in = (Bytef *) DataS.c_str();
        strm.avail_in = DataS.size() ;
        strm.total_out = 0;
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY)>=0) // 15 + 16 are magic values for gzip
        {
            do
            {
                strm.next_out = (unsigned char*) Buffer;
                strm.avail_out = Buffer_Size;
                if (deflate(&strm, Z_FINISH)<0)
                    break;
                file->write(Buffer, Buffer_Size-strm.avail_out);
            }
            while (strm.avail_out == 0);
            deflateEnd (&strm);
        }
        delete[] Buffer;

        file->flush();
        file->seek(0);
    }

    Q_EMIT statsFileGenerated(file, name);
}

//---------------------------------------------------------------------------
void FileInformation::Export_CSV (const QString &ExportFileName)
{
    if (ExportFileName.isEmpty())
        return;

    string StatsToExternalData=ReferenceStat()->StatsToCSV();

    QFile F(ExportFileName);
    F.open(QIODevice::WriteOnly|QIODevice::Truncate);
    F.write(StatsToExternalData.c_str(), StatsToExternalData.size());
}


//***************************************************************************
// Info
//***************************************************************************

//---------------------------------------------------------------------------
QByteArray FileInformation::Picture_Get (size_t Pos)
{
    if (!Glue || Pos>=ReferenceStat()->x_Current || Pos>=Glue->Thumbnails_Size(0))
        return QByteArray();

    return Glue->Thumbnail_Get(0, Pos);
}

QString FileInformation::fileName() const
{
    return FileName;
}

//---------------------------------------------------------------------------
int FileInformation::Frames_Count_Get (size_t Stats_Pos)
{
    if (Stats_Pos==(size_t)-1)
        Stats_Pos=ReferenceStream_Pos_Get();
    
    if (Stats_Pos>=Stats.size())
        return -1;
    return Stats[Stats_Pos]->x_Max[0];
}

//---------------------------------------------------------------------------
int FileInformation::Frames_Pos_Get (size_t Stats_Pos)
{
    // Looking for the first video stream
    if (Stats_Pos==(size_t)-1)
        Stats_Pos=ReferenceStream_Pos_Get();
    if (Stats_Pos>=Stats.size())
        return -1;

    int Pos;

    if (Stats_Pos!=ReferenceStream_Pos_Get())
    {
        // Computing frame pos based on the first stream
        double TimeStamp=ReferenceStat()->x[1][Frames_Pos];
        Pos=0;
        for (; Pos<Stats[Stats_Pos]->x_Current_Max; Pos++)
        {
            if (Stats[Stats_Pos]->x[1][Pos]>=TimeStamp)
            {
                if (Pos && Stats[Stats_Pos]->x[1][Pos]!=TimeStamp)
                    Pos--;
                break;
            }
        }
    }
    else
        Pos=Frames_Pos;
    
    return Pos;
}

QString FileInformation::Frame_Type_Get(size_t Stats_Pos, size_t frameIndex) const
{
    // Looking for the first video stream
    if (Stats_Pos==(size_t)-1)
        Stats_Pos=ReferenceStream_Pos_Get();
    if (Stats_Pos>=Stats.size())
        return QString();

    QString frameType;

    if(frameIndex == -1)
    {
        if (Stats_Pos!=ReferenceStream_Pos_Get())
        {
            // Computing frame pos based on the first stream
            double TimeStamp=ReferenceStat()->x[1][Frames_Pos];
            int Pos=0;
            for (; Pos<Stats[Stats_Pos]->x_Current_Max; Pos++)
            {
                if (Stats[Stats_Pos]->x[1][Pos]>=TimeStamp)
                {
                    if (Pos && Stats[Stats_Pos]->x[1][Pos]!=TimeStamp)
                        Pos--;

                    frameType = QString("%1").arg(Stats[Stats_Pos]->pict_type_char[Pos]);
                    break;
                }
            }
        }
        else
            frameType = QString("%1").arg(Stats[Stats_Pos]->pict_type_char[Frames_Pos]);
    } else {
        if(frameIndex <= Stats[Stats_Pos]->x_Current_Max)
            frameType = QString("%1").arg(Stats[Stats_Pos]->pict_type_char[frameIndex]);
    }

    return frameType;
}

//---------------------------------------------------------------------------
void FileInformation::Frames_Pos_Set (int Pos, size_t Stats_Pos)
{
    // Looking for the first video stream
    if (Stats_Pos==(size_t)-1)
        Stats_Pos=ReferenceStream_Pos_Get();
    if (Stats_Pos>=Stats.size())
        return;

    if (Stats_Pos!=ReferenceStream_Pos_Get())
    {
        // Computing frame pos based on the first stream
        double TimeStamp=Stats[Stats_Pos]->x[1][Pos];
        Pos=0;
        for (; Pos<ReferenceStat()->x_Current_Max; Pos++)
            if (ReferenceStat()->x[1][Pos]>=TimeStamp)
            {
                if (Pos && ReferenceStat()->x[1][Pos]!=TimeStamp)
                    Pos--;
                break;
            }
    }
    
    if (Pos<0)
        Pos=0;
    if (Pos>=ReferenceStat()->x_Current_Max)
        Pos=ReferenceStat()->x_Current_Max-1;

    if (Frames_Pos==Pos)
        return;
    Frames_Pos=Pos;

    Q_EMIT positionChanged();
}

//---------------------------------------------------------------------------
void FileInformation::Frames_Pos_Minus ()
{
    if (Frames_Pos==0)
        return;

    Frames_Pos--;

    Q_EMIT positionChanged();
}

//---------------------------------------------------------------------------
void FileInformation::Frames_Pos_Plus ()
{
    if (Frames_Pos+1>=ReferenceStat()->x_Current_Max)
        return;

    Frames_Pos++;

    Q_EMIT positionChanged();
}

//---------------------------------------------------------------------------
bool FileInformation::PlayBackFilters_Available ()
{
    return Glue;
}

qreal FileInformation::averageFrameRate() const
{
    if(!Glue)
        return 0;

    auto splitted = QString::fromStdString(Glue->AvgVideoFrameRate_Get()).split("/");
    if(splitted.length() == 1)
        return splitted[0].toDouble();

    return splitted[0].toDouble() / splitted[1].toDouble();
}

bool FileInformation::isValid() const
{
    return Glue != 0;
}

int FileInformation::BitsPerRawSample() const
{
    if(!Glue)
        return 0;

    return Glue->BitsPerRawSample_Get();
}

FileInformation::SignalServerCheckUploadedStatus FileInformation::signalServerCheckUploadedStatus() const
{
    if(checkFileUploadedOperation)
    {
        switch(checkFileUploadedOperation->state())
        {
        case CheckFileUploadedOperation::Unknown:
            return SignalServerCheckUploadedStatus::Checking;
        case CheckFileUploadedOperation::Uploaded:
            return SignalServerCheckUploadedStatus::Uploaded;
        case CheckFileUploadedOperation::NotUploaded:
            return SignalServerCheckUploadedStatus::NotUploaded;
        case CheckFileUploadedOperation::Error:
            return SignalServerCheckUploadedStatus::CheckError;
        }
    }

    return NotChecked;
}

QString FileInformation::signalServerCheckUploadedStatusString() const
{
    switch(signalServerCheckUploadedStatus())
    {
    case NotChecked:
        return "Not Checked";
    case Checking:
        return "Checking";
    case Uploaded:
        return "Uploaded";
    case NotUploaded:
        return "Not Uploaded";
    case CheckError:
        return "Check Error";
    default:
        return "Unknown";
    }
}

QString FileInformation::signalServerCheckUploadedStatusErrorString() const
{
    return checkFileUploadedOperation ? checkFileUploadedOperation->errorString() : QString();
}

FileInformation::SignalServerUploadStatus FileInformation::signalServerUploadStatus() const
{
    if(uploadOperation)
    {
        switch(uploadOperation->state())
        {
        case UploadFileOperation::Uploading:
            return SignalServerUploadStatus::Uploading;
        case UploadFileOperation::Uploaded:
            return SignalServerUploadStatus::Done;
        case UploadFileOperation::Error:
            return SignalServerUploadStatus::UploadError;
        }
    }

    return Idle;
}

QString FileInformation::signalServerUploadStatusString() const
{
    switch(signalServerUploadStatus())
    {
    case Idle:
        return "Idle";
    case Uploading:
        return "Uploading";
    case Done:
        return "Upload Done";
    case UploadError:
        return "Upload Error";
    default:
        return "Unknown";
    }
}

QString FileInformation::signalServerUploadStatusErrorString() const
{
    return uploadOperation ? uploadOperation->errorString() : QString();
}

int FileInformation::index() const
{
    return m_index;
}

void FileInformation::setIndex(int value)
{
    m_index = value;
}

void FileInformation::checkFileUploaded(const QString &fileName)
{
    if(thread() != QThread::currentThread())
    {
        QMetaObject::invokeMethod(this, "checkFileUploaded", Q_ARG(const QString&, fileName));
        return;
    }

    // on UI thread
    checkFileUploadedOperation = signalServer->checkFileUploaded(fileName);
    connect(checkFileUploadedOperation.data(), SIGNAL(finished()), this, SLOT(checkFileUploadedDone()));
}

void FileInformation::checkFileUploadedDone()
{
    if(checkFileUploadedOperation->state() == CheckFileUploadedOperation::NotUploaded)
    {
        qDebug() << "file not uploaded: " << checkFileUploadedOperation->fileName();

        if(signalServer->autoUpload() && m_parsed)
            handleAutoUpload();
    }
    else if(checkFileUploadedOperation->state() == CheckFileUploadedOperation::Uploaded)
    {
        qDebug() << "file is already uploaded: " << checkFileUploadedOperation->fileName();
    }
    else if(checkFileUploadedOperation->state() == CheckFileUploadedOperation::Error)
    {
        qDebug() << "file not uploaded: " << checkFileUploadedOperation->fileName() << ", error: " << checkFileUploadedOperation->errorString();
    }

    Q_EMIT signalServerCheckUploadedStatusChanged();
}

void FileInformation::upload(const QFileInfo& fileInfo)
{
    QString fullName = fileInfo.filePath();
    QSharedPointer<QFile> file = QSharedPointer<QFile>::create(fullName);
    if(file->open(QIODevice::ReadOnly))
    {
        upload(file, fileInfo.fileName());
    }
}

void FileInformation::upload(SharedFile file, const QString &fileName)
{
    uploadOperation = signalServer->uploadFile(fileName, file);
    connect(uploadOperation.data(), SIGNAL(finished()), this, SLOT(uploadDone()));
    connect(uploadOperation.data(), SIGNAL(uploadProgress(qint64, qint64)), this, SIGNAL(signalServerUploadProgressChanged(qint64, qint64)));

    Q_EMIT signalServerUploadStatusChanged();
}

void FileInformation::cancelUpload()
{
    if(uploadOperation)
        uploadOperation->cancel();
}

void FileInformation::uploadDone()
{
    Q_EMIT signalServerUploadStatusChanged();
}

void FileInformation::parsingDone(bool success)
{
    if(signalServerCheckUploadedStatus() == SignalServerCheckUploadedStatus::NotUploaded)
    {
        qDebug() << "parsing done: " << success;

        if(signalServer->autoUpload() && m_parsed)
            handleAutoUpload();
    }
}

void FileInformation::handleAutoUpload()
{
    QString statsFileName = fileName() + ".qctools.xml.gz";
    QFileInfo fileInfo(statsFileName);

    if(!fileInfo.exists())
    {
        connect(this, SIGNAL(statsFileGenerated(SharedFile, QString)), this, SLOT(upload(SharedFile,QString)), Qt::UniqueConnection);
        startExport();
    } else {
        SharedFile file = SharedFile(new QFile(statsFileName));
        if(file->open(QFile::ReadOnly))
            upload(file, fileInfo.fileName());
    }
}

bool FileInformation::parsed() const
{
    return m_parsed;
}
