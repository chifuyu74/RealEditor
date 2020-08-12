#include "PackageWindow.h"
#include "ObjectProperties.h"
#include "App.h"

#include <wx/menu.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/collpane.h>

#include <Tera/ALog.h>
#include <Tera/FPackage.h>
#include <Tera/FObjectResource.h>
#include <Tera/UObject.h>
#include <Tera/UClass.h>

#define FAKE_IMPORT_ROOT MININT
#define FAKE_EXPORT_ROOT MAXINT

enum ControlElementId {
	New = wxID_HIGHEST + 1,
	Open,
	Save,
	SaveAs,
	Close,
	Exit,
	LogWin,
	Back,
	Forward
};

wxDEFINE_EVENT(PACKAGE_READY, wxCommandEvent); 
wxDEFINE_EVENT(PACKAGE_ERROR, wxCommandEvent);

#include "PackageWindowLayout.h"

PackageWindow::PackageWindow(std::shared_ptr<FPackage>& package, App* application)
  : wxFrame(nullptr, wxID_ANY, application->GetAppDisplayName() + L" - " + A2W(package->GetSourcePath()))
  , Application(application)
  , Package(package)
{
	SetSizeHints(wxSize(1024, 700), wxDefaultSize);
	InitLayout();
	
	ObjectTreeCtrl->SetFocus();
	SetPropertiesHidden(true);
	SetContentHidden(true);

	wxPoint pos = Application->GetLastWindowPosition();
	if (pos.x == WIN_POS_FULLSCREEN)
	{
		Maximize();
	}
	else if (pos.x == WIN_POS_CENTER)
	{
		CenterOnScreen();
		Application->SetLastWindowPosition(GetPosition());
	}
	else
	{
		pos.x += 25; pos.y += 25;
		SetPosition(pos);
		Application->SetLastWindowPosition(pos);
	}
	OnNoneObjectSelected();
}

void PackageWindow::OnCloseWindow(wxCloseEvent& event)
{
	if (!Package->IsReady())
	{
		Package->CancelOperation();
	}
	Application->PackageWindowWillClose(this);
	wxFrame::OnCloseWindow(event);
}

PackageWindow::~PackageWindow()
{
	FPackage::UnloadPackage(Package);
	delete ImageList;
}

wxString PackageWindow::GetPackagePath() const
{
  return wxString(Package->GetSourcePath().String());
}

bool PackageWindow::OnObjectLoaded(const std::string& id)
{
	for (const auto p : Editors)
	{
		if (p.second->GetEditorId() == id)
		{
			if (ActiveEditor == p.second)
			{
				UpdateProperties(ActiveEditor->GetObject() ,ActiveEditor->GetObjectProperties());
			}
			p.second->OnObjectLoaded();
			return true;
		}
	}
	return false;
}

void PackageWindow::LoadObjectTree()
{
	DataModel = new ObjectTreeModel(Package->GetPackageName(), Package->GetRootExports(), Package->GetRootImports());
	ObjectTreeCtrl->AssociateModel(DataModel.get());
	wxDataViewColumn* col = new wxDataViewColumn("title", new wxDataViewIconTextRenderer, 1, wxDVC_DEFAULT_WIDTH, wxALIGN_LEFT);
	ObjectTreeCtrl->AppendColumn(col);
	col->SetWidth(ObjectTreeCtrl->GetSize().x - 4);
}

void PackageWindow::OnIdle(wxIdleEvent& e)
{
}

void PackageWindow::SidebarSplitterOnIdle(wxIdleEvent&)
{
	SidebarSplitter->SetSashPosition(230);
	SidebarSplitter->Disconnect(wxEVT_IDLE, wxIdleEventHandler(PackageWindow::SidebarSplitterOnIdle), NULL, this);
}

void PackageWindow::OnObjectTreeSelectItem(wxDataViewEvent& e)
{
	ObjectTreeNode* node = (ObjectTreeNode*)ObjectTreeCtrl->GetCurrentItem().GetID();
	if (!node || !node->GetParent())
	{
		OnNoneObjectSelected();
		return;
	}
	PACKAGE_INDEX index = node->GetObjectIndex();
	if (index < 0)
	{
		OnImportObjectSelected(index);
	}
	else
	{
		OnExportObjectSelected(index);
	}
}

void PackageWindow::OnImportObjectSelected(INT index)
{
	ShowEditor(nullptr);
	if (index == FAKE_IMPORT_ROOT)
	{
		ObjectTitleLabel->SetLabelText("No selection");
		SetPropertiesHidden(true);
		SetContentHidden(true);
		return;
	}
	FObjectImport* obj = Package->GetImportObject(index);
	ObjectTitleLabel->SetLabelText(wxString::Format("%ls (%ls)", obj->GetObjectName().WString().c_str(), obj->GetClassName().WString().c_str()));
	SetPropertiesHidden(true);
	SetContentHidden(true);
}

void PackageWindow::OnExportObjectSelected(INT index)
{
	if (index == FAKE_EXPORT_ROOT)
	{
		ShowEditor(nullptr);
		ObjectTitleLabel->SetLabelText("No selection");
		SetPropertiesHidden(true);
		SetContentHidden(true);
		return;
	}
	
	FObjectExport* fobj = Package->GetExportObject(index);
	ObjectTitleLabel->SetLabelText(wxString::Format("%ls (%ls)", fobj->GetObjectName().WString().c_str(), fobj->GetClassName().WString().c_str()));
	ObjectSizeLabel->SetLabelText(wxString::Format("0x%08X", fobj->SerialSize));
	ObjectOffsetLabel->SetLabelText(wxString::Format("0x%08X", fobj->SerialOffset));
	std::string flags = ObjectFlagsToString(fobj->ObjectFlags);
	ObjectFlagsTextfield->SetLabelText(flags);
	flags = ExportFlagsToString(fobj->ExportFlags);
	ExportFlagsTextfield->SetLabelText(flags);
	SetPropertiesHidden(false);
	SetContentHidden(false);

	{
		auto it = Editors.find(index);
		if (it != Editors.end())
		{
			ShowEditor(it->second);
			it->second->LoadObject();
		}
		else
		{
			UObject* object = fobj->Object;
			if (!object)
			{
				Package->GetObject(index, false);
				object = fobj->Object;
			}
			GenericEditor* e = GenericEditor::CreateEditor(EditorContainer, this, object);
			ShowEditor(e);
			e->LoadObject();
		}
	}
}

void PackageWindow::UpdateProperties(UObject* object, std::vector<FPropertyTag*> properties)
{
	if (!PropertyRootCategory)
	{
		PropertyRootCategory = new wxPropertyCategory(object->GetObjectName().WString());
		PropertyRootCategory->SetValue(object->GetClassName().String());
		PropertiesCtrl->Append(PropertyRootCategory);
	}
	else
	{
		PropertyRootCategory->DeleteChildren();
		PropertyRootCategory->SetLabel(object->GetObjectName().WString());
		PropertyRootCategory->SetValue(object->GetClassName().String());
	}

	CreateProperty(PropertiesCtrl, PropertyRootCategory, properties);

	PropertiesCtrl->RefreshGrid();
	PropertyRootCategory->RefreshChildren();
}

void PackageWindow::OnNoneObjectSelected()
{
	ObjectTitleLabel->SetLabelText("No selection");
	SetPropertiesHidden(true);
	SetContentHidden(true);
	ShowEditor(nullptr);
}

void PackageWindow::OnNewClicked(wxCommandEvent& e)
{

}

void PackageWindow::OnOpenClicked(wxCommandEvent& e)
{
	Application->ShowOpenDialog();
}

void PackageWindow::OnSaveClicked(wxCommandEvent& e)
{

}

void PackageWindow::OnSaveAsClicked(wxCommandEvent& e)
{

}

void PackageWindow::OnCloseClicked(wxCommandEvent& e)
{
	Close();
}

void PackageWindow::OnExitClicked(wxCommandEvent&)
{
	Application->ExitMainLoop();
}

void PackageWindow::OnToggleLogClicked(wxCommandEvent&)
{
	bool isShown = ALog::IsShown();
	ALog::Show(!isShown);
	Application->GetConfig().LogConfig.ShowLog = !isShown;
}

void PackageWindow::OnMoveEnd(wxMoveEvent& e)
{
	if (IsMaximized())
	{
		Application->SetLastWindowPosition(wxPoint(WIN_POS_FULLSCREEN, 0));
	}
	else
	{
		Application->SetLastWindowPosition(GetPosition());
	}
	e.Skip();
}

void PackageWindow::OnMaximized(wxMaximizeEvent& e)
{
	Application->SetLastWindowPosition(wxPoint(WIN_POS_FULLSCREEN, 0));
	e.Skip();
}

void PackageWindow::OnPackageReady(wxCommandEvent&)
{
	ObjectTreeCtrl->Freeze();
	LoadObjectTree();
	ObjectTreeCtrl->Thaw();
}

void PackageWindow::OnPackageError(wxCommandEvent& e)
{
	wxMessageBox("Failed to load the package!", e.GetString(), wxICON_ERROR);
	FPackage::UnloadPackage(Package);
	Close();
}

void PackageWindow::OnObjectTreeStartEdit(wxDataViewEvent& e)
{
	wxDataViewItem item = e.GetItem();
	if (ObjectTreeCtrl->IsExpanded(item))
	{
		ObjectTreeCtrl->Collapse(item);
	}
	else
	{
		ObjectTreeCtrl->Expand(item);
	}
	e.Veto();
}

wxBEGIN_EVENT_TABLE(PackageWindow, wxFrame)
EVT_MENU(ControlElementId::New, PackageWindow::OnNewClicked)
EVT_MENU(ControlElementId::Open, PackageWindow::OnOpenClicked)
EVT_MENU(ControlElementId::Save, PackageWindow::OnSaveClicked)
EVT_MENU(ControlElementId::SaveAs, PackageWindow::OnSaveAsClicked)
EVT_MENU(ControlElementId::Close, PackageWindow::OnCloseClicked)
EVT_MENU(ControlElementId::Exit, PackageWindow::OnExitClicked)
EVT_MENU(ControlElementId::LogWin, PackageWindow::OnToggleLogClicked)
EVT_DATAVIEW_ITEM_START_EDITING(wxID_ANY, PackageWindow::OnObjectTreeStartEdit)
EVT_DATAVIEW_SELECTION_CHANGED(wxID_ANY, PackageWindow::OnObjectTreeSelectItem)
EVT_MOVE_END(PackageWindow::OnMoveEnd)
EVT_MAXIMIZE(PackageWindow::OnMaximized)
EVT_CLOSE(PackageWindow::OnCloseWindow)
EVT_COMMAND(wxID_ANY, PACKAGE_READY, PackageWindow::OnPackageReady)
EVT_COMMAND(wxID_ANY, PACKAGE_ERROR, PackageWindow::OnPackageError)
EVT_IDLE(PackageWindow::OnIdle)
wxEND_EVENT_TABLE()