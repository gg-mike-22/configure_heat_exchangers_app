﻿#include "explodewidget.h"

#include "explodemanager.h"
#include <QFileDialog>
#include <QToolButton>
#include <QMenu>
#include <qevent.h>

#include <attr_product.h>
#include <instance.h>
#include <conv_predefined.h>
#include <conv_model_exchange.h>
#include <conv_exchange_settings.h>
#include <vsn_postprocessing.h>
#include <qt_aboutscenewidget.h>
#include <qpushbutton.h>

#include <last.h>

//-----------------------------------------------------------------------------
// ---
static void _selectSegment(SelectionManagerPtr ptrSelectManager, SceneSegment* pSegment)
{
    if (pSegment->GetRepresentation() == nullptr)
    {
        auto listItem = pSegment->GetChildSegments();
        for (auto it = listItem.begin(); it != listItem.end(); ++it)
        {
            SceneSegment* pSegmentItem = (*it);
            if (pSegmentItem->GetRepresentation() == nullptr)
                _selectSegment(ptrSelectManager, pSegmentItem);
            else
                ptrSelectManager->Select(pSegmentItem->GetUniqueKey());
        }
    }
}

//-----------------------------------------------------------------------------
// ---
static void fillSegmentList(ExplodeTreeView* pTreeWidget, SceneSegment* pSegment, SceneSegment* pPrentSegment)
{
    pTreeWidget->slotAppendItem(pSegment, pPrentSegment);
    auto listItem = pSegment->GetChildSegments();
    for (auto it = listItem.begin(); it != listItem.end(); ++it)
    {
        SceneSegment* pSegmentItem = (*it);
        fillSegmentList(pTreeWidget, pSegmentItem, pSegment);
    }
}

/* ExplodeWidget */
ExplodeWidget::ExplodeWidget(QWidget* pParent)
    : QtOpenGLSceneWidget(pParent)
    , m_pModel(nullptr)
    , m_pSegmModel(nullptr)
    , m_pSceneGenerator(new SceneGenerator())
    , m_pExplodeManager(new ExplodeManager(this))
    , m_ptrSelectManager(std::make_shared<SelectionManager>())
    , m_pTreeWidget(nullptr)
{
    QtVision::ProcessTypes ptTypes = QtVision::pt_Pan | QtVision::pt_Zoom | QtVision::pt_Rotate;
    QtVision::createProcessesCameraControls(this, ptTypes);
}

ExplodeWidget::~ExplodeWidget()
{
    VSN_DELETE_AND_NULL(m_pSceneGenerator);
    VSN_DELETE_AND_NULL(m_pExplodeManager);
}

QGroupBox* ExplodeWidget::createGroupExplode(QWidget& widget, const int heightButton, const std::string& mainTabName)
{
    return m_pExplodeManager->createGroupExplode(widget, heightButton, mainTabName);
}

//-----------------------------------------------------------------------------
// 
// ---
void ExplodeWidget::initializeGL()
{
    QtOpenGLSceneWidget::initializeGL();

    Light* pLight = graphicsScene()->CreateExtraLight();
    pLight->SetType(Light::DirectionLight);
    pLight->SetPosition(-100.0, -1.0, -1.0);
    pLight->SetDoubleSided(false);

    // Render Statistics
    setAboutSceneWidget(new QtVision::QtAboutSceneWidget(this));
    RenderStatistics::SetActivation(true);

    GlobalFlags::SetPixelCullingEnabled(true);

    GlobalFlags::SetFaceCulling(CullFaces::Back);
    viewport()->GetCamera()->SetViewOrientation(Orientation::IsoXYZ);
    viewport()->SetGradientBackgroundColour(QtVision::topColor, QtVision::bottomColor);
    viewport()->SetPixelCullingSize(35);
    mainLight()->SetDoubleSided(false);
    graphicsView()->SetSmoothTransition(true);

    // надо написать manager для процессов
    ObjectPickSelection* pPickSelection = objectPickSelection();
    m_ptrSelectManager->SetSceneContent(sceneContent());
    m_ptrSelectManager->SetObjectPickSelection(pPickSelection);
    m_ptrSelectManager->SetSelectionMode(SelectionManager::MultiSelection);
    m_ptrSelectManager->SetDynamicHighlighting(true);
    m_ptrSelectManager->SetBodySelectionEnabled(true);

    Object::Connect(m_ptrSelectManager.get(), &SelectionManager::signalCurrentItemsModified, this, &ExplodeWidget::slotCurrentItemsModified);
    Object::Connect(m_ptrSelectManager.get(), &SelectionManager::signalItemSelectModified, this, &ExplodeWidget::slotItemSelectModified);
    Object::Connect(m_ptrSelectManager.get(), &SelectionManager::signalStateModified, this, &QtOpenGLWidget::updateWidget);
}

//-----------------------------------------------------------------------------
//
// ---
void ExplodeWidget::openModel()
{
    SceneSegment* pTopSegment = sceneContent()->GetRootSegment();
    Q_ASSERT(pTopSegment != nullptr);

    const QString lastUserPath;
    QStringList filters = QtVision::openSaveFilters();
    QString oneLineFilters = filters.join("\n");
#ifdef Q_OS_WIN
    const QStringList files = m_modelPath.empty()
        ? QFileDialog::getOpenFileNames(this, tr("Select Models"), lastUserPath, oneLineFilters)
        : QStringList(m_modelPath.c_str());
#else 
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Select Models"), lastUserPath, oneLineFilters, nullptr, QFileDialog::DontUseNativeDialog);
#endif
    if (files.count() > 0)
    {
        if (files.count() > 1)
            emit setVisibleProgress(true);
        if (m_pSegmModel != nullptr)
        {
            pTopSegment->RemoveSegment(m_pSegmModel);
            VSN_DELETE_AND_NULL(m_pSegmModel);
            m_pTreeWidget->clear();
        }

        if (m_pModel != nullptr)
            m_pModel.reset();
    }
    else
        return;

    if (m_pModel != nullptr)
        Q_ASSERT(false);

    QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
    loadFiles(files);
    if (m_pModel->ItemsCount() == 0)
    {
        m_pModel.reset();
        exit(1);
    }
    createScene();
    fillGeometryList();
}

//-----------------------------------------------------------------------------
//
// ---
void ExplodeWidget::showContextMenu(const QPoint& pos)
{
    QMenu menu;
    QAction* hideAction = menu.addAction("Hide");
    hideAction->setEnabled(m_ptrSelectManager->GetSelectionList().size() > 0);
    QAction* showOnlyAction = menu.addAction("Show only");
    QAction* showAllAction = menu.addAction("Show all");
    menu.addSeparator();
    QAction* clearSelectionAction = menu.addAction("Clear selection");
    QAction* selectedAction = menu.exec(pos);

    if (selectedAction == hideAction)
        hideSelectedObjects();
    else if (selectedAction == showOnlyAction)
        showAllObjects();
    else if (selectedAction == clearSelectionAction)
        clearSelectionObjects();
}

//-----------------------------------------------------------------------------
//
// ---
ExplodeTreeView* ExplodeWidget::createGeometryList(QWidget* pParent)
{
    m_pTreeWidget = new ExplodeTreeView(this, pParent);
    QObject::connect(m_pTreeWidget, &QTreeWidget::currentItemChanged, this, &ExplodeWidget::currentItemChanged);
    QObject::connect(m_pTreeWidget, &QTreeWidget::itemChanged, this, &ExplodeWidget::itemChanged);
    return m_pTreeWidget;
}

//-----------------------------------------------------------------------------
//
// ---
void ExplodeWidget::currentItemChanged(QTreeWidgetItem* current, QTreeWidgetItem* previous)
{
    Q_UNUSED(previous);
    if (m_pSegmModel == nullptr)
        return;
    m_ptrSelectManager->UnselectAll();
    if (TreeObjectItem* pWidgetItem = dynamic_cast<TreeObjectItem*>(current))
    {
        SceneSegment* pCurrentSegment = pWidgetItem->object();
        if (m_pExplodeManager->isSelectionEnabled())
        {
            if (!m_pExplodeManager->onSelectItem(pCurrentSegment))
            {
                m_ptrSelectManager->UnselectAll();
                return;
            }
            update();
        }
        if (pCurrentSegment->GetRepresentation() == nullptr)
            _selectSegment(m_ptrSelectManager, pCurrentSegment);
        else
            m_ptrSelectManager->Select(pCurrentSegment->GetUniqueKey());
    }
}

//-----------------------------------------------------------------------------
// 
// ---
void ExplodeWidget::itemChanged(QTreeWidgetItem* item)
{
    if (TreeObjectItem* pWidgetItem = dynamic_cast<TreeObjectItem*>(item))
    {
        SceneSegment* pCurrentSegment = pWidgetItem->object();
        pCurrentSegment->SetVisible(item->checkState(0) == Qt::Checked);
        update();
    }
}

//-----------------------------------------------------------------------------
// 
// ---
void ExplodeWidget::contextMenuEvent(QContextMenuEvent* event)
{
//    showContextMenu(event->globalPos());
}

//-----------------------------------------------------------------------------
// 
// ---
void ExplodeWidget::loadFiles(const QStringList& files)
{
    m_pModel = new MbModel();
    emit setProgressRange(0, files.count());
    for (int i = 0, count = files.count(); i < count; ++i)
    {
        QString fileName = files.at(i);
        if (fileName.isEmpty())
            continue;

        MbModel model;
        ConvConvertorProperty3D convProperties;
        convProperties.enableAutostitch = true;
        convProperties.autostitchPrecision = Math::visualSag;
        convProperties.fileName = c3d::WToPathstring(fileName.toStdWString());
        if (c3d::ImportFromFile(model, c3d::WToPathstring(fileName.toStdWString()), &convProperties, nullptr) == cnv_Success)
        {
            for (MbModel::ItemIterator iter = model.Begin(), last = model.End(); iter != last; ++iter)
                m_pModel->AddItem(*(*iter));
        }
        emit buildProgress(i);
    }
    emit resetProgress();
    emit setVisibleProgress(false);
}

//-----------------------------------------------------------------------------
// 
// ---
void ExplodeWidget::createScene()
{
    SceneSegment* pTopSegment = sceneContent()->GetRootSegment();
    Q_ASSERT(pTopSegment != nullptr);
    ProgressBuild* pProgressBuild = m_pSceneGenerator->CreateProgressBuild();
    Object::Connect(pProgressBuild, &ProgressBuild::BuildAllCompleted, this, &ExplodeWidget::slotFinishBuildRep);
    m_pSegmModel = m_pSceneGenerator->CreateSceneSegment(m_pModel, pTopSegment, false);
    m_pSceneGenerator->StartBuildGeometry();
}

//-----------------------------------------------------------------------------
// 
// ---
void ExplodeWidget::fillGeometryList()
{
    m_pTreeWidget->setCurrentItem(nullptr);
    m_pTreeWidget->clear();
    if (m_pSegmModel != nullptr)
        fillSegmentList(m_pTreeWidget, m_pSegmModel, nullptr);
}

//-----------------------------------------------------------------------------
// 
// ---
void ExplodeWidget::hideSelectedObjects()
{
    auto lst = m_ptrSelectManager->GetSelectionList();
    for (SelectionItem* item : lst)
    {
        InstSelectionItem* pItem = static_cast<InstSelectionItem*>(item);
        TreeObjectItem* treeItem = m_pTreeWidget->findItemByObject(sceneContent()->GetSegment(pItem->GetNodeKey()));
        treeItem->setCheckState(0, Qt::Unchecked);
    }
}

//-----------------------------------------------------------------------------
// 
// ---
void ExplodeWidget::showAllObjects()
{
}

//-----------------------------------------------------------------------------
// 
// ---
void ExplodeWidget::clearSelectionObjects()
{
}

//-----------------------------------------------------------------------------
// 
// ---
void ExplodeWidget::slotFinishBuildRep()
{
    QApplication::restoreOverrideCursor();
    viewport()->ZoomToFit(sceneContent()->GetBoundingBox());
    m_pExplodeManager->init(m_pSegmModel, m_pTreeWidget);
    update();
}

//-----------------------------------------------------------------------------
//
// ---
void ExplodeWidget::setRenderingMode()
{
    if (QToolButton* pToolButton = qobject_cast<QToolButton*>(sender()))
    {
        if (pToolButton->objectName() == QLatin1String("Perspective"))
            viewport()->SetOrthographicProjection(!pToolButton->isChecked());
    }
    update();
}

//-----------------------------------------------------------------------------
//
// ---
void ExplodeWidget::slotItemSelectModified()
{
}

//-----------------------------------------------------------------------------
//
// ---
void ExplodeWidget::slotCurrentItemsModified(std::list<SelectionItem*>& oldItems, std::list<SelectionItem*>& newItems)
{
    std::vector<const SceneSegment*> oldSegments;
    if (oldItems.size() == newItems.size())
    {
        for (auto pSelectionItem : oldItems)
        {
            InstSelectionItem* pItem = static_cast<InstSelectionItem*>(pSelectionItem);
            const SceneSegment* pSegm = pItem->GetSceneSegment();
            oldSegments.push_back(pSegm);
        }
    }
    VSN_UNUSED(oldItems);
    if (!newItems.empty())
    {
        for (auto pSelectionItem : newItems)
        {
            InstSelectionItem* pItem = static_cast<InstSelectionItem*>(pSelectionItem);
            const SceneSegment* pSegm = pItem->GetSceneSegment();
            TreeObjectItem* treeItem = m_pTreeWidget->findItemByObject(pSegm);

            const bool isNewItem = std::find(oldSegments.cbegin(), oldSegments.cend(), pSegm) == oldSegments.cend();
            if (isNewItem)
            {
                if (pSegm->HasRep())
                    m_pExplodeManager->onSelectItem(pSegm);
            }
            bool blockSig = m_pTreeWidget->blockSignals(true);
            m_pTreeWidget->setCurrentItem(treeItem);
            m_pTreeWidget->blockSignals(blockSig);
        }
    }
    else
    {
        m_pTreeWidget->setCurrentItem(nullptr);
        m_pExplodeManager->onSelectItem(nullptr);
    }
    update();
}