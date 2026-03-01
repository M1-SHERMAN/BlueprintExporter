#include "BlueprintGraphExtractor.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"

#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Timeline.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_Composite.h"

FExportedBlueprint FBlueprintGraphExtractor::Extract(UBlueprint* Blueprint)
{
	FExportedBlueprint Result;

	if (!Blueprint)
	{
		return Result;
	}

	Result.BlueprintName = Blueprint->GetName();

	if (Blueprint->ParentClass)
	{
		FString ParentName = Blueprint->ParentClass->GetName();
		// Remove _C suffix from blueprint generated classes
		if (ParentName.EndsWith(TEXT("_C")))
		{
			ParentName.LeftChopInline(2);
		}
		Result.ParentClass = ParentName;
	}

	// Variables
	for (const FBPVariableDescription& VarDesc : Blueprint->NewVariables)
	{
		FExportedVariable Var;
		Var.Name = VarDesc.VarName.ToString();
		Var.Type = ResolveVariableType(VarDesc.VarType);

		// Container type
		switch (VarDesc.VarType.ContainerType)
		{
		case EPinContainerType::Array:
			Var.ContainerType = TEXT("Array");
			break;
		case EPinContainerType::Set:
			Var.ContainerType = TEXT("Set");
			break;
		case EPinContainerType::Map:
			Var.ContainerType = TEXT("Map");
			break;
		default:
			break;
		}

		// Default value with enum resolution
		if (!VarDesc.DefaultValue.IsEmpty())
		{
			Var.DefaultValue = VarDesc.DefaultValue;

			if (VarDesc.VarType.PinSubCategoryObject.IsValid())
			{
				UEnum* EnumPtr = Cast<UEnum>(VarDesc.VarType.PinSubCategoryObject.Get());
				if (EnumPtr)
				{
					int64 EnumValue = EnumPtr->GetValueByNameString(Var.DefaultValue);
					if (EnumValue != INDEX_NONE)
					{
						FText DisplayName = EnumPtr->GetDisplayNameTextByValue(EnumValue);
						if (!DisplayName.IsEmpty())
						{
							Var.DefaultValue = DisplayName.ToString();
						}
					}
				}
			}
		}

		// Fallback: read default value from CDO when FBPVariableDescription::DefaultValue is empty
		if (Var.DefaultValue.IsEmpty() && Blueprint->GeneratedClass)
		{
			UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject(false);
			if (CDO)
			{
				FProperty* Prop = Blueprint->GeneratedClass->FindPropertyByName(VarDesc.VarName);
				if (Prop)
				{
					FString ExportedValue;
					Prop->ExportText_InContainer(0, ExportedValue, CDO, nullptr, nullptr, PPF_None);
					if (!ExportedValue.IsEmpty())
					{
						Var.DefaultValue = ExportedValue;

						// Resolve enum display names from exported string
						// ExportText may produce "E_AI_State::NewEnumerator0" — strip prefix first
						UEnum* EnumPtr = Cast<UEnum>(VarDesc.VarType.PinSubCategoryObject.Get());
						if (EnumPtr)
						{
							FString RawName = Var.DefaultValue;
							int32 ColonIdx;
							if (RawName.FindLastChar(':', ColonIdx))
							{
								RawName = RawName.Mid(ColonIdx + 1);
							}
							int64 EnumValue = EnumPtr->GetValueByNameString(RawName);
							if (EnumValue != INDEX_NONE)
							{
								FText DisplayName = EnumPtr->GetDisplayNameTextByValue(EnumValue);
								if (!DisplayName.IsEmpty())
								{
									Var.DefaultValue = DisplayName.ToString();
								}
							}
						}
					}
				}
			}
		}

		// Flags from PropertyFlags
		if (VarDesc.PropertyFlags & CPF_Edit)
		{
			if (VarDesc.PropertyFlags & CPF_DisableEditOnInstance)
			{
				Var.Flags.Add(TEXT("EditDefaultsOnly"));
			}
			else
			{
				Var.Flags.Add(TEXT("EditAnywhere"));
			}
		}

		if (VarDesc.PropertyFlags & CPF_BlueprintVisible)
		{
			if (VarDesc.PropertyFlags & CPF_BlueprintReadOnly)
			{
				Var.Flags.Add(TEXT("BlueprintReadOnly"));
			}
			else
			{
				Var.Flags.Add(TEXT("BlueprintReadWrite"));
			}
		}

		if (VarDesc.PropertyFlags & CPF_Net)
		{
			Var.Flags.Add(TEXT("Replicated"));
		}

		Result.Variables.Add(MoveTemp(Var));
	}

	// EventGraph(s)
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph)
		{
			Result.Graphs.Add(ExtractGraph(Graph, TEXT("EventGraph")));
		}
	}

	// Function graphs
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph)
		{
			Result.Graphs.Add(ExtractGraph(Graph, TEXT("Function")));
		}
	}

	// Macro graphs
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		if (Graph)
		{
			Result.Graphs.Add(ExtractGraph(Graph, TEXT("Macro")));
		}
	}

	// Interface graphs
	for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
	{
		for (UEdGraph* Graph : InterfaceDesc.Graphs)
		{
			if (Graph)
			{
				Result.Graphs.Add(ExtractGraph(Graph, TEXT("Interface")));
			}
		}
	}

	return Result;
}

FExportedGraph FBlueprintGraphExtractor::ExtractGraph(UEdGraph* Graph, const FString& GraphType)
{
	FExportedGraph Result;
	Result.GraphName = Graph->GetName();
	Result.GraphType = GraphType;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}

		// Skip comment nodes
		if (Cast<UEdGraphNode_Comment>(Node))
		{
			continue;
		}

		Result.Nodes.Add(ExtractNode(Node, Result.GraphName));
	}

	return Result;
}

FExportedNode FBlueprintGraphExtractor::ExtractNode(UEdGraphNode* Node, const FString& GraphName)
{
	FExportedNode Result;
	Result.NodeName = Node->GetName();
	Result.NodeClass = Node->GetClass()->GetName();
	Result.GraphName = GraphName;

	ExtractNodeProperties(Node, Result);

	// Extract subgraph for Composite (collapsed) nodes
	if (UK2Node_Composite* CompositeNode = Cast<UK2Node_Composite>(Node))
	{
		if (CompositeNode->BoundGraph)
		{
			Result.SubGraph = MakeShared<FExportedGraph>(
				ExtractGraph(CompositeNode->BoundGraph, TEXT("Collapsed")));
		}
	}

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin)
		{
			Result.Pins.Add(ExtractPin(Pin));
		}
	}

	return Result;
}

void FBlueprintGraphExtractor::ExtractNodeProperties(UEdGraphNode* Node, FExportedNode& OutNode)
{
	// UK2Node_ComponentBoundEvent (must be before UK2Node_Event — it inherits from it)
	if (UK2Node_ComponentBoundEvent* CompEventNode = Cast<UK2Node_ComponentBoundEvent>(Node))
	{
		OutNode.Properties.Emplace(TEXT("ComponentProperty"), CompEventNode->ComponentPropertyName.ToString());
		OutNode.Properties.Emplace(TEXT("DelegateProperty"), CompEventNode->DelegatePropertyName.ToString());
		return;
	}

	// UK2Node_CustomEvent (must be before UK2Node_Event — it inherits from it)
	if (UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(Node))
	{
		OutNode.Properties.Emplace(TEXT("Event"), CustomEventNode->CustomFunctionName.ToString());
		return;
	}

	// UK2Node_Event
	if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
	{
		OutNode.Properties.Emplace(TEXT("Event"),
			EventNode->EventReference.GetMemberName().ToString());
		if (EventNode->bOverrideFunction)
		{
			OutNode.Properties.Emplace(TEXT("Override"), TEXT("true"));
		}
		return;
	}

	// UK2Node_CallFunction (also covers UK2Node_CallArrayFunction)
	if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
	{
		FString FuncName = CallNode->FunctionReference.GetMemberName().ToString();

		// If not self context, prefix with parent class
		if (!CallNode->FunctionReference.IsSelfContext())
		{
			UClass* MemberParent = CallNode->FunctionReference.GetMemberParentClass();
			if (MemberParent)
			{
				FString ClassName = MemberParent->GetName();
				FuncName = FString::Printf(TEXT("%s::%s"), *ClassName, *FuncName);
			}
		}

		OutNode.Properties.Emplace(TEXT("Function"), FuncName);
		return;
	}

	// UK2Node_VariableGet / UK2Node_VariableSet (both inherit UK2Node_Variable)
	if (UK2Node_VariableGet* VarGetNode = Cast<UK2Node_VariableGet>(Node))
	{
		OutNode.Properties.Emplace(TEXT("Variable"),
			VarGetNode->VariableReference.GetMemberName().ToString());
		return;
	}

	if (UK2Node_VariableSet* VarSetNode = Cast<UK2Node_VariableSet>(Node))
	{
		OutNode.Properties.Emplace(TEXT("Variable"),
			VarSetNode->VariableReference.GetMemberName().ToString());
		return;
	}

	// UK2Node_DynamicCast
	if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
	{
		if (CastNode->TargetType)
		{
			FString TargetName = CastNode->TargetType->GetName();
			if (TargetName.EndsWith(TEXT("_C")))
			{
				TargetName.LeftChopInline(2);
			}
			OutNode.Properties.Emplace(TEXT("CastTo"), TargetName);
		}
		return;
	}

	// UK2Node_MacroInstance
	if (UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
	{
		UEdGraph* MacroGraph = MacroNode->GetMacroGraph();
		if (MacroGraph)
		{
			OutNode.Properties.Emplace(TEXT("Macro"), MacroGraph->GetName());
		}
		return;
	}

	// UK2Node_Timeline
	if (UK2Node_Timeline* TimelineNode = Cast<UK2Node_Timeline>(Node))
	{
		OutNode.Properties.Emplace(TEXT("Timeline"), TimelineNode->TimelineName.ToString());
		return;
	}

	// UK2Node_SwitchEnum
	if (UK2Node_SwitchEnum* SwitchNode = Cast<UK2Node_SwitchEnum>(Node))
	{
		if (SwitchNode->Enum)
		{
			OutNode.Properties.Emplace(TEXT("Enum"), SwitchNode->Enum->GetName());
		}
		return;
	}

	// UK2Node_Composite
	if (UK2Node_Composite* CompositeNode = Cast<UK2Node_Composite>(Node))
	{
		if (CompositeNode->BoundGraph)
		{
			OutNode.Properties.Emplace(TEXT("Collapsed"), CompositeNode->BoundGraph->GetName());
		}
		return;
	}

	// Other node types: no additional properties needed
}

FExportedPin FBlueprintGraphExtractor::ExtractPin(UEdGraphPin* Pin)
{
	FExportedPin Result;

	// Name: prefer display name, fallback to PinName
	FText DisplayName = Pin->GetDisplayName();
	Result.Name = DisplayName.IsEmpty()
		? Pin->PinName.ToString()
		: DisplayName.ToString();

	// Direction
	Result.Direction = (Pin->Direction == EGPD_Output) ? TEXT("Output") : TEXT("Input");

	// Category
	Result.Category = Pin->PinType.PinCategory.ToString();

	// SubType
	if (Pin->PinType.PinSubCategoryObject.IsValid())
	{
		UObject* SubObj = Pin->PinType.PinSubCategoryObject.Get();
		if (SubObj)
		{
			Result.SubType = SubObj->GetName();
			// Remove _C suffix from blueprint generated classes
			if (Result.SubType.EndsWith(TEXT("_C")))
			{
				Result.SubType.LeftChopInline(2);
			}
		}
	}
	else if (!Pin->PinType.PinSubCategory.IsNone())
	{
		// float, double, etc. via PinSubCategory
		Result.SubType = Pin->PinType.PinSubCategory.ToString();
	}

	// Container type
	switch (Pin->PinType.ContainerType)
	{
	case EPinContainerType::Array:
		Result.ContainerType = TEXT("Array");
		break;
	case EPinContainerType::Set:
		Result.ContainerType = TEXT("Set");
		break;
	case EPinContainerType::Map:
		Result.ContainerType = TEXT("Map");
		break;
	default:
		break;
	}

	// Default value
	if (!Pin->DefaultValue.IsEmpty())
	{
		Result.DefaultValue = Pin->DefaultValue;

		// Strip _C suffix for class/object type pins
		const FName& Cat = Pin->PinType.PinCategory;
		if ((Cat == TEXT("class") || Cat == TEXT("object"))
			&& Result.DefaultValue.EndsWith(TEXT("_C")))
		{
			Result.DefaultValue.LeftChopInline(2);
		}

		// Resolve enum display names (e.g. "NewEnumerator0" -> "Idle")
		if (Pin->PinType.PinSubCategoryObject.IsValid())
		{
			UEnum* EnumPtr = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get());
			if (EnumPtr)
			{
				int64 EnumValue = EnumPtr->GetValueByNameString(Result.DefaultValue);
				if (EnumValue != INDEX_NONE)
				{
					FText Name = EnumPtr->GetDisplayNameTextByValue(EnumValue);
					if (!Name.IsEmpty())
					{
						Result.DefaultValue = Name.ToString();
					}
				}
			}
		}
	}
	else if (Pin->DefaultObject != nullptr)
	{
		Result.DefaultValue = Pin->DefaultObject->GetName();
		if (Result.DefaultValue.EndsWith(TEXT("_C")))
		{
			Result.DefaultValue.LeftChopInline(2);
		}
	}

	// Hidden
	Result.bIsHidden = Pin->bHidden;

	// Connections
	for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
	{
		if (!LinkedPin || !LinkedPin->GetOwningNode())
		{
			continue;
		}

		FString TargetNodeName = LinkedPin->GetOwningNode()->GetName();
		FText TargetDisplayName = LinkedPin->GetDisplayName();
		FString TargetPinName = TargetDisplayName.IsEmpty()
			? LinkedPin->PinName.ToString()
			: TargetDisplayName.ToString();

		Result.LinkedTo.Add(TPair<FString, FString>(TargetNodeName, TargetPinName));
	}

	return Result;
}

FExportedGraph FBlueprintGraphExtractor::ExtractSelectedNodes(
	const TSet<UEdGraphNode*>& SelectedNodes,
	const UEdGraph* OwningGraph)
{
	FExportedGraph Result;
	Result.GraphName = OwningGraph ? OwningGraph->GetName() : TEXT("Unknown");
	Result.GraphType = TEXT("Selection");

	for (UEdGraphNode* Node : SelectedNodes)
	{
		if (!Node)
		{
			continue;
		}

		// Skip comment nodes
		if (Cast<UEdGraphNode_Comment>(Node))
		{
			continue;
		}

		Result.Nodes.Add(ExtractNode(Node, Result.GraphName));
	}

	return Result;
}

FString FBlueprintGraphExtractor::ResolveVariableType(const FEdGraphPinType& PinType)
{
	if (PinType.PinSubCategoryObject.IsValid())
	{
		UObject* SubObj = PinType.PinSubCategoryObject.Get();
		if (SubObj)
		{
			FString TypeName = SubObj->GetName();
			if (TypeName.EndsWith(TEXT("_C")))
			{
				TypeName.LeftChopInline(2);
			}
			return TypeName;
		}
	}

	if (!PinType.PinSubCategory.IsNone())
	{
		return PinType.PinSubCategory.ToString();
	}

	return PinType.PinCategory.ToString();
}
