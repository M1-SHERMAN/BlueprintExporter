#include "BlueprintTextFormatter.h"

FString FBlueprintTextFormatter::Format(const FExportedBlueprint& Blueprint)
{
	TArray<FString> Lines;

	// Header
	if (!Blueprint.ParentClass.IsEmpty())
	{
		Lines.Add(FString::Printf(TEXT("=== Blueprint: %s (Parent: %s) ==="),
			*Blueprint.BlueprintName, *Blueprint.ParentClass));
	}
	else
	{
		Lines.Add(FString::Printf(TEXT("=== Blueprint: %s ==="), *Blueprint.BlueprintName));
	}
	Lines.Add(TEXT(""));

	// Variables section
	if (Blueprint.Variables.Num() > 0)
	{
		FString VarSection = FormatVariables(Blueprint.Variables);
		if (!VarSection.IsEmpty())
		{
			Lines.Add(VarSection);
			Lines.Add(TEXT(""));
		}
	}

	for (const FExportedGraph& Graph : Blueprint.Graphs)
	{
		FString GraphOutput = FormatGraph(Graph);
		if (!GraphOutput.IsEmpty())
		{
			Lines.Add(GraphOutput);
		}
	}

	return FString::Join(Lines, TEXT("\n")).TrimEnd();
}

FString FBlueprintTextFormatter::FormatVariables(const TArray<FExportedVariable>& Variables)
{
	if (Variables.Num() == 0)
	{
		return FString();
	}

	TArray<FString> Lines;
	Lines.Add(TEXT("=== Variables ==="));

	for (const FExportedVariable& Var : Variables)
	{
		FString FullType = Var.Type;
		if (!Var.ContainerType.IsEmpty())
		{
			FullType = FString::Printf(TEXT("%s<%s>"), *Var.ContainerType, *Var.Type);
		}

		FString Line = FString::Printf(TEXT("  %s : %s"), *Var.Name, *FullType);

		if (!Var.DefaultValue.IsEmpty() && !IsTrivialDefault(Var.DefaultValue))
		{
			Line += FString::Printf(TEXT(" = %s"), *Var.DefaultValue);
		}

		// Omit flags if they are the default combination (EditDefaultsOnly + BlueprintReadWrite)
		bool bIsDefaultFlagCombo = (Var.Flags.Num() == 2
			&& Var.Flags.Contains(TEXT("EditDefaultsOnly"))
			&& Var.Flags.Contains(TEXT("BlueprintReadWrite")));

		if (Var.Flags.Num() > 0 && !bIsDefaultFlagCombo)
		{
			Line += FString::Printf(TEXT("  [%s]"), *FString::Join(Var.Flags, TEXT(", ")));
		}

		Lines.Add(Line);
	}

	return FString::Join(Lines, TEXT("\n"));
}

FString FBlueprintTextFormatter::FormatGraph(const FExportedGraph& Graph)
{
	if (Graph.Nodes.Num() == 0)
	{
		return FString();
	}

	TArray<FString> Lines;

	// Graph header
	if (Graph.GraphType == TEXT("EventGraph"))
	{
		Lines.Add(FString::Printf(TEXT("--- Graph: %s ---"), *Graph.GraphName));
	}
	else
	{
		Lines.Add(FString::Printf(TEXT("--- Graph: %s (%s) ---"),
			*Graph.GraphName, *Graph.GraphType));
	}
	Lines.Add(TEXT(""));

	// Build node map
	TMap<FString, const FExportedNode*> NodeMap;
	for (const FExportedNode& Node : Graph.Nodes)
	{
		NodeMap.Add(Node.NodeName, &Node);
	}

	// Topological sort
	TArray<FExportedNode> SortedNodes = TopologicalSort(Graph.Nodes);

	// Format each node
	for (const FExportedNode& Node : SortedNodes)
	{
		if (Node.NodeClass == TEXT("K2Node_Knot"))
		{
			continue;
		}
		Lines.Add(FormatNode(Node));
	}

	// Execution flow
	FString ExecFlow = FormatExecutionFlow(SortedNodes, NodeMap);
	if (!ExecFlow.IsEmpty())
	{
		Lines.Add(TEXT(""));                         // node block → Flow separator
		Lines.Add(TEXT("=== Execution Flow ==="));
		Lines.Add(ExecFlow);
		Lines.Add(TEXT(""));                         // Flow trailing blank (Graph separator)
	}
	else
	{
		Lines.Add(TEXT(""));                         // no Flow: keep trailing blank (Graph separator)
	}

	return FString::Join(Lines, TEXT("\n"));
}

FString FBlueprintTextFormatter::FormatNode(const FExportedNode& Node)
{
	TArray<FString> Lines;

	// Compact node header: [SemanticTitle] (ShortId)
	FString SemanticTitle = GetSemanticTitle(Node);
	FString ShortId = GetShortNodeId(Node.NodeName);
	Lines.Add(FString::Printf(TEXT("[%s] (%s)"), *SemanticTitle, *ShortId));

	// Node properties -- skip those consumed by GetSemanticTitle and Override
	static const TSet<FString> ConsumedKeys = {
		TEXT("Event"), TEXT("Function"), TEXT("Variable"), TEXT("CastTo"),
		TEXT("Macro"), TEXT("Timeline"), TEXT("Enum"), TEXT("Collapsed"),
		TEXT("ComponentProperty"), TEXT("DelegateProperty"), TEXT("Operator"),
	};

	bool bHasOverride = false;
	for (const auto& Prop : Node.Properties)
	{
		if (Prop.Key == TEXT("SelfContext"))
		{
			continue;
		}
		if (Prop.Key == TEXT("Override") && Prop.Value == TEXT("true"))
		{
			bHasOverride = true;
			continue;
		}
		if (ConsumedKeys.Contains(Prop.Key))
		{
			continue;
		}
		Lines.Add(FString::Printf(TEXT("  %s: %s"), *Prop.Key, *Prop.Value));
	}
	if (bHasOverride)
	{
		Lines.Add(TEXT("  (Override)"));
	}

	// Meaningful pins
	TArray<FExportedPin> VisiblePins = GetMeaningfulPins(Node);
	for (const FExportedPin& Pin : VisiblePins)
	{
		FString PinStr = FormatPin(Pin);
		if (!PinStr.IsEmpty())
		{
			Lines.Add(FString::Printf(TEXT("  %s"), *PinStr));
		}
	}

	// Render expanded subgraph for Composite nodes
	if (Node.SubGraph)
	{
		FString SubContent = FormatGraph(*Node.SubGraph);
		if (!SubContent.IsEmpty())
		{
			TArray<FString> SubLines;
			SubContent.ParseIntoArrayLines(SubLines);
			for (const FString& SubLine : SubLines)
			{
				Lines.Add(FString::Printf(TEXT("  %s"), *SubLine));
			}
		}
	}

	return FString::Join(Lines, TEXT("\n"));
}

FString FBlueprintTextFormatter::FormatPin(const FExportedPin& Pin)
{
	// Direction arrow
	const TCHAR* Arrow = Pin.Direction == TEXT("Output") ? TEXT("\u2192") : TEXT("\u2190");

	// Type string: prefer SubType, fallback to Category
	FString TypeStr = Pin.Category;
	if (!Pin.SubType.IsEmpty())
	{
		TypeStr = Pin.SubType;
	}
	if (!Pin.ContainerType.IsEmpty())
	{
		TypeStr = FString::Printf(TEXT("%s<%s>"), *Pin.ContainerType, *TypeStr);
	}

	// Suppress type annotation for primitive types (reduces noise)
	static const TSet<FString> PrimitiveTypes = {
		TEXT("bool"), TEXT("int"), TEXT("int64"), TEXT("float"), TEXT("double"),
		TEXT("byte"), TEXT("real"), TEXT("string"), TEXT("text"), TEXT("name"),
	};
	bool bShowType = !PrimitiveTypes.Contains(TypeStr);

	// Default value: only show non-trivial defaults; exec/delegate never show
	FString Default;
	if (!Pin.DefaultValue.IsEmpty()
		&& !IsTrivialDefault(Pin.DefaultValue)
		&& Pin.Category != TEXT("exec")
		&& Pin.Category != TEXT("delegate"))
	{
		Default = FString::Printf(TEXT(" = %s"), *Pin.DefaultValue);
	}

	// Connection targets
	FString Connection;
	if (Pin.LinkedTo.Num() > 0)
	{
		TArray<FString> Targets;
		for (const auto& Link : Pin.LinkedTo)
		{
			FString TargetStr = FString::Printf(TEXT("%s.%s"), *Link.Key, *Link.Value);
			if (!SelectionContext.IsEmpty() && !SelectionContext.Contains(Link.Key))
			{
				TargetStr += TEXT(" (external)");
			}
			Targets.Add(TargetStr);
		}
		Connection = FString::Printf(TEXT(" -> %s"), *FString::Join(Targets, TEXT(", ")));
	}

	if (bShowType)
	{
		return FString::Printf(TEXT("%s %s (%s)%s%s"),
			Arrow, *Pin.Name, *TypeStr, *Default, *Connection);
	}
	else
	{
		return FString::Printf(TEXT("%s %s%s%s"),
			Arrow, *Pin.Name, *Default, *Connection);
	}
}

FString FBlueprintTextFormatter::GetReadableType(const FString& NodeClass) const
{
	static TMap<FString, FString> TypeMap;
	if (TypeMap.Num() == 0)
	{
		TypeMap.Add(TEXT("K2Node_Event"), TEXT("EVENT"));
		TypeMap.Add(TEXT("K2Node_CustomEvent"), TEXT("CUSTOM_EVENT"));
		TypeMap.Add(TEXT("K2Node_CallFunction"), TEXT("CALL"));
		TypeMap.Add(TEXT("K2Node_CallArrayFunction"), TEXT("CALL(Array)"));
		TypeMap.Add(TEXT("K2Node_VariableGet"), TEXT("GET"));
		TypeMap.Add(TEXT("K2Node_VariableSet"), TEXT("SET"));
		TypeMap.Add(TEXT("K2Node_IfThenElse"), TEXT("BRANCH"));
		TypeMap.Add(TEXT("K2Node_SwitchEnum"), TEXT("SWITCH"));
		TypeMap.Add(TEXT("K2Node_SwitchInteger"), TEXT("SWITCH(Int)"));
		TypeMap.Add(TEXT("K2Node_SwitchString"), TEXT("SWITCH(String)"));
		TypeMap.Add(TEXT("K2Node_MacroInstance"), TEXT("MACRO"));
		TypeMap.Add(TEXT("K2Node_DynamicCast"), TEXT("CAST"));
		TypeMap.Add(TEXT("K2Node_Knot"), TEXT("REROUTE"));
		TypeMap.Add(TEXT("K2Node_FunctionEntry"), TEXT("FUNC_ENTRY"));
		TypeMap.Add(TEXT("K2Node_FunctionResult"), TEXT("FUNC_RESULT"));
		TypeMap.Add(TEXT("K2Node_MakeArray"), TEXT("MAKE_ARRAY"));
		TypeMap.Add(TEXT("K2Node_MakeStruct"), TEXT("MAKE_STRUCT"));
		TypeMap.Add(TEXT("K2Node_BreakStruct"), TEXT("BREAK_STRUCT"));
		TypeMap.Add(TEXT("K2Node_Select"), TEXT("SELECT"));
		TypeMap.Add(TEXT("K2Node_SpawnActorFromClass"), TEXT("SPAWN_ACTOR"));
		TypeMap.Add(TEXT("K2Node_Timeline"), TEXT("TIMELINE"));
		TypeMap.Add(TEXT("K2Node_Delay"), TEXT("DELAY"));
		TypeMap.Add(TEXT("K2Node_ForEachLoop"), TEXT("FOR_EACH"));
		TypeMap.Add(TEXT("K2Node_CommutativeAssociativeBinaryOperator"), TEXT("OPERATOR"));
		TypeMap.Add(TEXT("K2Node_PromotableOperator"), TEXT("OPERATOR"));
		TypeMap.Add(TEXT("K2Node_ComponentBoundEvent"), TEXT("COMPONENT_EVENT"));
		TypeMap.Add(TEXT("K2Node_Composite"), TEXT("COLLAPSED"));
		TypeMap.Add(TEXT("K2Node_Tunnel"), TEXT("TUNNEL"));
	}

	const FString* Found = TypeMap.Find(NodeClass);
	if (Found)
	{
		return *Found;
	}

	// Fallback: strip K2Node_ prefix
	FString Result = NodeClass;
	Result.RemoveFromStart(TEXT("K2Node_"));
	return Result;
}

FString FBlueprintTextFormatter::GetShortNodeId(const FString& NodeName)
{
	FString Id = NodeName;
	Id.RemoveFromStart(TEXT("K2Node_"));

	// Compress common class prefixes to shorter forms
	static const TArray<TPair<FString, FString>> Replacements = {
		{ TEXT("CallFunction"), TEXT("CallFunc") },
		{ TEXT("CallArrayFunction"), TEXT("CallArrFunc") },
		{ TEXT("VariableGet"), TEXT("VarGet") },
		{ TEXT("VariableSet"), TEXT("VarSet") },
		{ TEXT("IfThenElse"), TEXT("ITE") },
		{ TEXT("FunctionEntry"), TEXT("FuncEntry") },
		{ TEXT("FunctionResult"), TEXT("FuncResult") },
		{ TEXT("SwitchEnum"), TEXT("Switch") },
		{ TEXT("SwitchInteger"), TEXT("SwitchInt") },
		{ TEXT("SwitchString"), TEXT("SwitchStr") },
		{ TEXT("DynamicCast"), TEXT("Cast") },
		{ TEXT("CustomEvent"), TEXT("CustEvent") },
		{ TEXT("GetSubsystem"), TEXT("GetSub") },
		{ TEXT("MacroInstance"), TEXT("Macro") },
		{ TEXT("CommutativeAssociativeBinaryOperator"), TEXT("Op") },
		{ TEXT("PromotableOperator"), TEXT("Op") },
		{ TEXT("ComponentBoundEvent"), TEXT("CompEvent") },
		{ TEXT("ExecutionSequence"), TEXT("Seq") },
		{ TEXT("EnumEquality"), TEXT("EnumEq") },
	};

	for (const auto& Rep : Replacements)
	{
		if (Id.StartsWith(Rep.Key))
		{
			Id = Rep.Value + Id.Mid(Rep.Key.Len());
			break;
		}
	}

	return Id;
}

FString FBlueprintTextFormatter::GetSemanticTitle(const FExportedNode& Node) const
{
	const FString& NodeClass = Node.NodeClass;

	// Find primary property value (first non-Override, non-SelfContext property)
	FString PrimaryValue;
	for (const auto& Prop : Node.Properties)
	{
		if (Prop.Key == TEXT("SelfContext") || Prop.Key == TEXT("Override"))
		{
			continue;
		}
		PrimaryValue = Prop.Value;
		break;
	}

	if (PrimaryValue.IsEmpty())
	{
		// No primary property -- use readable type
		return GetReadableType(NodeClass);
	}

	// Build title based on node class + property
	if (NodeClass == TEXT("K2Node_Event") || NodeClass == TEXT("K2Node_CustomEvent"))
	{
		return PrimaryValue;
	}
	if (NodeClass == TEXT("K2Node_CallFunction") || NodeClass == TEXT("K2Node_CallArrayFunction"))
	{
		return PrimaryValue;
	}
	if (NodeClass == TEXT("K2Node_VariableGet"))
	{
		return FString::Printf(TEXT("Get: %s"), *PrimaryValue);
	}
	if (NodeClass == TEXT("K2Node_VariableSet"))
	{
		return FString::Printf(TEXT("Set: %s"), *PrimaryValue);
	}
	if (NodeClass == TEXT("K2Node_DynamicCast"))
	{
		return FString::Printf(TEXT("Cast: %s"), *PrimaryValue);
	}
	if (NodeClass == TEXT("K2Node_SwitchEnum") || NodeClass == TEXT("K2Node_SwitchInteger")
		|| NodeClass == TEXT("K2Node_SwitchString"))
	{
		return FString::Printf(TEXT("Switch: %s"), *PrimaryValue);
	}
	if (NodeClass == TEXT("K2Node_MacroInstance"))
	{
		return FString::Printf(TEXT("Macro: %s"), *PrimaryValue);
	}
	if (NodeClass == TEXT("K2Node_Timeline"))
	{
		return FString::Printf(TEXT("Timeline: %s"), *PrimaryValue);
	}
	if (NodeClass == TEXT("K2Node_Composite"))
	{
		return FString::Printf(TEXT("Collapsed: %s"), *PrimaryValue);
	}
	if (NodeClass == TEXT("K2Node_ComponentBoundEvent"))
	{
		// Has ComponentProperty and DelegateProperty -- find DelegateProperty
		FString DelegateName;
		for (const auto& Prop : Node.Properties)
		{
			if (Prop.Key == TEXT("DelegateProperty"))
			{
				DelegateName = Prop.Value;
				break;
			}
		}
		if (!DelegateName.IsEmpty())
		{
			return FString::Printf(TEXT("On %s.%s"), *PrimaryValue, *DelegateName);
		}
		return PrimaryValue;
	}
	if (NodeClass == TEXT("K2Node_CommutativeAssociativeBinaryOperator")
		|| NodeClass == TEXT("K2Node_PromotableOperator"))
	{
		return FString::Printf(TEXT("Op: %s"), *PrimaryValue);
	}
	if (NodeClass == TEXT("K2Node_IfThenElse"))
	{
		return TEXT("Branch");
	}

	// Fallback: readable type
	return GetReadableType(NodeClass);
}

TArray<FExportedNode> FBlueprintTextFormatter::TopologicalSort(const TArray<FExportedNode>& Nodes)
{
	// Build node name to index map
	TMap<FString, int32> NameToIndex;
	for (int32 i = 0; i < Nodes.Num(); ++i)
	{
		NameToIndex.Add(Nodes[i].NodeName, i);
	}

	// Build exec flow directed graph and compute in-degrees
	TMap<int32, TArray<int32>> ExecGraph;
	TMap<int32, int32> InDegree;

	for (int32 i = 0; i < Nodes.Num(); ++i)
	{
		ExecGraph.FindOrAdd(i);
		InDegree.FindOrAdd(i) = 0;
	}

	for (int32 i = 0; i < Nodes.Num(); ++i)
	{
		const FExportedNode& Node = Nodes[i];
		for (const FExportedPin& Pin : Node.Pins)
		{
			if (Pin.Category == TEXT("exec") && Pin.Direction == TEXT("Output") && Pin.LinkedTo.Num() > 0)
			{
				for (const auto& Link : Pin.LinkedTo)
				{
					const int32* TargetIdx = NameToIndex.Find(Link.Key);
					if (TargetIdx)
					{
						ExecGraph[i].Add(*TargetIdx);
						InDegree[*TargetIdx]++;
					}
				}
			}
		}
	}

	// Kahn's algorithm
	TArray<int32> Queue;
	for (const auto& Pair : InDegree)
	{
		if (Pair.Value == 0)
		{
			Queue.Add(Pair.Key);
		}
	}

	TArray<int32> SortedIndices;
	while (Queue.Num() > 0)
	{
		// Sort queue: Event nodes first, then by name
		Queue.Sort([&Nodes](int32 A, int32 B)
		{
			bool AIsEvent = Nodes[A].NodeClass.Contains(TEXT("Event"));
			bool BIsEvent = Nodes[B].NodeClass.Contains(TEXT("Event"));
			if (AIsEvent != BIsEvent)
			{
				return AIsEvent; // Events come first
			}
			return Nodes[A].NodeName < Nodes[B].NodeName;
		});

		int32 Current = Queue[0];
		Queue.RemoveAt(0);
		SortedIndices.Add(Current);

		for (int32 Neighbor : ExecGraph[Current])
		{
			InDegree[Neighbor]--;
			if (InDegree[Neighbor] == 0)
			{
				Queue.Add(Neighbor);
			}
		}
	}

	// Append remaining nodes not in sorted result (cycles or disconnected)
	TSet<int32> SortedSet(SortedIndices);
	for (int32 i = 0; i < Nodes.Num(); ++i)
	{
		if (!SortedSet.Contains(i))
		{
			SortedIndices.Add(i);
		}
	}

	TArray<FExportedNode> Result;
	for (int32 Idx : SortedIndices)
	{
		Result.Add(Nodes[Idx]);
	}
	return Result;
}

TArray<FExportedPin> FBlueprintTextFormatter::GetMeaningfulPins(const FExportedNode& Node)
{
	TArray<FExportedPin> Meaningful;

	for (const FExportedPin& Pin : Node.Pins)
	{
		// Always skip hidden self/Target pin
		if (Pin.bIsHidden && (Pin.Name == TEXT("Target") || Pin.Name == TEXT("self")))
		{
			continue;
		}

		// Skip Output_Get pin if not connected
		if (Pin.Name == TEXT("Output_Get") && Pin.LinkedTo.Num() == 0)
		{
			continue;
		}

		// Skip all pins of Reroute nodes (shown in flow summary instead)
		if (Node.NodeClass == TEXT("K2Node_Knot"))
		{
			continue;
		}

		// Skip exec input pins — exec output from source already declares this connection
		if (Pin.Category == TEXT("exec") && Pin.Direction == TEXT("Input"))
		{
			continue;
		}

		// Exec pins with no connections
		if (Pin.Category == TEXT("exec") && Pin.LinkedTo.Num() == 0)
		{
			// Keep named exec outputs (e.g. Branch true/false, Is Valid/Is Not Valid)
			static const TSet<FString> StandardExecNames = {
				TEXT("execute"), TEXT("then"), TEXT("OutputDelegate")
			};
			if (!StandardExecNames.Contains(Pin.Name))
			{
				Meaningful.Add(Pin);
			}
			continue;
		}

		// Skip internal engine pins (e.g. Default__KismetMathLibrary on SwitchEnum)
		if (Pin.DefaultValue.StartsWith(TEXT("Default__")) && Pin.LinkedTo.Num() == 0)
		{
			continue;
		}

		// Skip LatentActionInfo pins — always present on latent nodes, default value is meaningless
		if (Pin.SubType == TEXT("LatentActionInfo") && Pin.LinkedTo.Num() == 0)
		{
			continue;
		}

		// Skip unconnected delegate pins (e.g. CustomEvent Output Delegate)
		if (Pin.Category == TEXT("delegate") && Pin.LinkedTo.Num() == 0)
		{
			continue;
		}

		// Skip hidden + no connection + no default value pins
		if (Pin.bIsHidden && Pin.LinkedTo.Num() == 0 && Pin.DefaultValue.IsEmpty())
		{
			continue;
		}

		// Skip unconnected non-exec output pins with no meaningful default (not consumed by any node)
		if (Pin.Direction == TEXT("Output")
			&& Pin.Category != TEXT("exec")
			&& Pin.LinkedTo.Num() == 0
			&& (Pin.DefaultValue.IsEmpty() || IsTrivialDefault(Pin.DefaultValue)))
		{
			continue;
		}

		Meaningful.Add(Pin);
	}

	return Meaningful;
}

bool FBlueprintTextFormatter::IsTrivialDefault(const FString& Value)
{
	static const TSet<FString> TrivialValues = {
		TEXT("0"),
		TEXT("0.0"),
		TEXT("0.000000"),
		TEXT("0, 0, 0"),
		TEXT("false"),
		TEXT("None"),
		TEXT("()"),                                    // empty struct / delegate
		TEXT("(())"),                                  // empty array
		TEXT("(X=0.000000,Y=0.000000,Z=0.000000)"),   // zero vector
	};
	return TrivialValues.Contains(Value);
}

TPair<FString, FString> FBlueprintTextFormatter::ResolveRerouteChain(const FString& NodeName, const FString& OriginalPinName, const TMap<FString, const FExportedNode*>& NodeMap)
{
	TSet<FString> Visited;
	FString Current = NodeName;
	FString CurrentPinName = OriginalPinName;

	while (true)
	{
		const FExportedNode* const* NodePtr = NodeMap.Find(Current);
		if (!NodePtr || !*NodePtr)
		{
			break;
		}

		const FExportedNode& Node = **NodePtr;
		if (Node.NodeClass != TEXT("K2Node_Knot"))
		{
			break;
		}

		if (Visited.Contains(Current))
		{
			break;
		}
		Visited.Add(Current);

		// Find the output pin and follow it
		bool bFoundNext = false;
		for (const FExportedPin& Pin : Node.Pins)
		{
			if (Pin.Direction == TEXT("Output") && Pin.LinkedTo.Num() > 0)
			{
				Current = Pin.LinkedTo[0].Key;
				CurrentPinName = Pin.LinkedTo[0].Value;
				bFoundNext = true;
				break;
			}
		}

		if (!bFoundNext)
		{
			break;
		}
	}

	return TPair<FString, FString>(Current, CurrentPinName);
}

FString FBlueprintTextFormatter::FormatExecutionFlow(const TArray<FExportedNode>& Nodes, const TMap<FString, const FExportedNode*>& NodeMap)
{
	TArray<FString> Lines;

	for (const FExportedNode& Node : Nodes)
	{
		if (Node.NodeClass == TEXT("K2Node_Knot"))
		{
			continue;
		}

		for (const FExportedPin& Pin : Node.Pins)
		{
			if (Pin.Category != TEXT("exec") || Pin.Direction != TEXT("Output") || Pin.LinkedTo.Num() == 0)
			{
				continue;
			}

			for (const auto& Link : Pin.LinkedTo)
			{
				// Resolve reroute chain
				auto [FinalTarget, _] = ResolveRerouteChain(Link.Key, Link.Value, NodeMap);

				// In selection mode, skip connections to external nodes
				if (!SelectionContext.IsEmpty() && !SelectionContext.Contains(FinalTarget))
				{
					continue;
				}

				FString Label;
				if (Pin.Name != TEXT("then") && Pin.Name != TEXT("OutputPin"))
				{
					Label = FString::Printf(TEXT(" [%s]"), *Pin.Name);
				}

				// Use semantic titles for both source and target
				FString SourceTitle = GetSemanticTitle(Node);
				FString TargetTitle = FinalTarget; // fallback
				const FExportedNode* const* TargetNodePtr = NodeMap.Find(FinalTarget);
				if (TargetNodePtr && *TargetNodePtr)
				{
					TargetTitle = GetSemanticTitle(**TargetNodePtr);
				}

				Lines.Add(FString::Printf(TEXT("  %s%s --> %s"),
					*SourceTitle, *Label, *TargetTitle));
			}
		}
	}

	return FString::Join(Lines, TEXT("\n"));
}

FString FBlueprintTextFormatter::FormatGraphOnly(const FExportedGraph& Graph)
{
	// SelectionContext is empty (default), so FormatGraph runs in full mode
	return FormatGraph(Graph);
}

FString FBlueprintTextFormatter::FormatSummary(const FExportedBlueprint& Blueprint)
{
	TArray<FString> Lines;

	// Blueprint header (same style as Format())
	if (Blueprint.ParentClass.IsEmpty())
	{
		Lines.Add(FString::Printf(TEXT("=== Blueprint: %s ==="), *Blueprint.BlueprintName));
	}
	else
	{
		Lines.Add(FString::Printf(TEXT("=== Blueprint: %s (Parent: %s) ==="),
			*Blueprint.BlueprintName, *Blueprint.ParentClass));
	}
	Lines.Add(TEXT(""));

	// Variables section (reuse existing FormatVariables)
	if (Blueprint.Variables.Num() > 0)
	{
		FString VarSection = FormatVariables(Blueprint.Variables);
		if (!VarSection.IsEmpty())
		{
			Lines.Add(VarSection);
			Lines.Add(TEXT(""));
		}
	}

	// Graph list table
	Lines.Add(TEXT("=== Graphs ==="));

	for (const FExportedGraph& G : Blueprint.Graphs)
	{
		Lines.Add(FString::Printf(TEXT("  %s (%s) %d nodes"),
			*G.GraphName, *G.GraphType, G.Nodes.Num()));
	}

	return FString::Join(Lines, TEXT("\n")).TrimEnd();
}

FString FBlueprintTextFormatter::FormatSelectedNodes(const FExportedGraph& Graph, const FString& BlueprintName)
{
	if (Graph.Nodes.Num() == 0)
	{
		return FString();
	}

	// Populate selection context with all node names in the selection
	SelectionContext.Empty();
	for (const FExportedNode& Node : Graph.Nodes)
	{
		SelectionContext.Add(Node.NodeName);
	}

	TArray<FString> Lines;

	// Header
	Lines.Add(FString::Printf(TEXT("=== Selected Nodes from: %s / %s (%d nodes) ==="),
		*BlueprintName, *Graph.GraphName, Graph.Nodes.Num()));
	Lines.Add(TEXT(""));

	// Build node map
	TMap<FString, const FExportedNode*> NodeMap;
	for (const FExportedNode& Node : Graph.Nodes)
	{
		NodeMap.Add(Node.NodeName, &Node);
	}

	// Topological sort
	TArray<FExportedNode> SortedNodes = TopologicalSort(Graph.Nodes);

	// Format each node
	for (const FExportedNode& Node : SortedNodes)
	{
		if (Node.NodeClass == TEXT("K2Node_Knot"))
		{
			continue;
		}
		Lines.Add(FormatNode(Node));
	}

	// Execution flow
	FString ExecFlow = FormatExecutionFlow(SortedNodes, NodeMap);
	if (!ExecFlow.IsEmpty())
	{
		Lines.Add(TEXT(""));
		Lines.Add(TEXT("=== Execution Flow ==="));
		Lines.Add(ExecFlow);
		Lines.Add(TEXT(""));
	}
	else
	{
		Lines.Add(TEXT(""));
	}

	// Clear selection context so subsequent Format() calls are not affected
	SelectionContext.Empty();

	return FString::Join(Lines, TEXT("\n")).TrimEnd();
}

// ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Compact Mode
// ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

FString FBlueprintTextFormatter::FormatCompactBlueprint(const FExportedBlueprint& Blueprint)
{
	TArray<FString> Lines;

	Lines.Add(FString::Printf(TEXT("=== %s (Parent: %s) ==="),
		*Blueprint.BlueprintName,
		Blueprint.ParentClass.IsEmpty() ? TEXT("none") : *Blueprint.ParentClass));
	Lines.Add(TEXT(""));

	for (const FExportedGraph& Graph : Blueprint.Graphs)
	{
		FString CompactGraph = FormatCompactGraph(Graph);
		if (!CompactGraph.IsEmpty())
		{
			Lines.Add(CompactGraph);
			Lines.Add(TEXT(""));
		}
	}

	return FString::Join(Lines, TEXT("\n")).TrimEnd();
}

bool FBlueprintTextFormatter::HasExecPins(const FExportedNode& Node) const
{
	for (const FExportedPin& Pin : Node.Pins)
	{
		if (Pin.Category == TEXT("exec"))
		{
			return true;
		}
	}
	return false;
}

FString FBlueprintTextFormatter::ExtractFunctionSignature(const FExportedGraph& Graph)
{
	FString Params;
	FString Returns;

	for (const FExportedNode& Node : Graph.Nodes)
	{
		if (Node.NodeClass == TEXT("K2Node_FunctionEntry"))
		{
			// Output pins (non-exec) are the function parameters
			TArray<FString> ParamList;
			for (const FExportedPin& Pin : Node.Pins)
			{
				if (Pin.Category == TEXT("exec") || Pin.Direction != TEXT("Output"))
				{
					continue;
				}
				FString TypeStr = Pin.SubType.IsEmpty() ? Pin.Category : Pin.SubType;
				if (!Pin.ContainerType.IsEmpty())
				{
					TypeStr = FString::Printf(TEXT("%s<%s>"), *Pin.ContainerType, *TypeStr);
				}
				ParamList.Add(FString::Printf(TEXT("%s:%s"), *Pin.Name, *TypeStr));
			}
			Params = FString::Join(ParamList, TEXT(", "));
		}
		else if (Node.NodeClass == TEXT("K2Node_FunctionResult"))
		{
			// Only extract returns once (first FunctionResult found)
			if (!Returns.IsEmpty())
			{
				continue;
			}
			TArray<FString> RetList;
			for (const FExportedPin& Pin : Node.Pins)
			{
				if (Pin.Category == TEXT("exec") || Pin.Direction != TEXT("Input"))
				{
					continue;
				}
				FString TypeStr = Pin.SubType.IsEmpty() ? Pin.Category : Pin.SubType;
				if (!Pin.ContainerType.IsEmpty())
				{
					TypeStr = FString::Printf(TEXT("%s<%s>"), *Pin.ContainerType, *TypeStr);
				}
				RetList.Add(FString::Printf(TEXT("%s:%s"), *Pin.Name, *TypeStr));
			}
			if (RetList.Num() > 0)
			{
				Returns = FString::Join(RetList, TEXT(", "));
			}
		}
	}

	FString Sig = FString::Printf(TEXT("%s(%s)"), *Graph.GraphName, *Params);
	if (!Returns.IsEmpty())
	{
		Sig += FString::Printf(TEXT(" -> %s"), *Returns);
	}
	return Sig;
}

FString FBlueprintTextFormatter::FormatCompactGraph(const FExportedGraph& Graph)
{
	if (Graph.Nodes.Num() == 0)
	{
		return FString();
	}

	TArray<FString> Lines;

	// Build node map
	TMap<FString, const FExportedNode*> NodeMap;
	for (const FExportedNode& Node : Graph.Nodes)
	{
		NodeMap.Add(Node.NodeName, &Node);
	}

	// Determine graph type and build signature
	bool bIsFunction = (Graph.GraphType == TEXT("Function") || Graph.GraphType == TEXT("Macro")
		|| Graph.GraphType == TEXT("Interface"));

	if (bIsFunction)
	{
		FString Sig = ExtractFunctionSignature(Graph);
		Lines.Add(FString::Printf(TEXT("=== %s ==="), *Sig));
	}
	else
	{
		// EventGraph: each event is an entry point
		Lines.Add(FString::Printf(TEXT("--- %s ---"), *Graph.GraphName));
	}

	// Find entry points: FunctionEntry, Event, CustomEvent, ComponentBoundEvent nodes
	TArray<const FExportedNode*> EntryNodes;
	for (const FExportedNode& Node : Graph.Nodes)
	{
		if (Node.NodeClass == TEXT("K2Node_FunctionEntry")
			|| Node.NodeClass == TEXT("K2Node_Event")
			|| Node.NodeClass == TEXT("K2Node_CustomEvent")
			|| Node.NodeClass == TEXT("K2Node_ComponentBoundEvent"))
		{
			EntryNodes.Add(&Node);
		}
	}

	// Sort entries: FunctionEntry first, then events alphabetically by semantic title
	EntryNodes.Sort([this](const FExportedNode& A, const FExportedNode& B)
	{
		if (A.NodeClass == TEXT("K2Node_FunctionEntry")) return true;
		if (B.NodeClass == TEXT("K2Node_FunctionEntry")) return false;
		return GetSemanticTitle(A) < GetSemanticTitle(B);
	});

	// DFS from each entry point
	for (const FExportedNode* Entry : EntryNodes)
	{
		TSet<FString> Visited;

		// For events, print the event name as a header
		if (Entry->NodeClass != TEXT("K2Node_FunctionEntry"))
		{
			FString EventTitle = GetSemanticTitle(*Entry);
			Lines.Add(FString::Printf(TEXT("[%s]:"), *EventTitle));
		}

		// Find the exec output (the "then" pin) and follow it
		for (const FExportedPin& Pin : Entry->Pins)
		{
			if (Pin.Category == TEXT("exec") && Pin.Direction == TEXT("Output")
				&& Pin.LinkedTo.Num() > 0)
			{
				auto [FinalTarget, _] = ResolveRerouteChain(
					Pin.LinkedTo[0].Key, Pin.LinkedTo[0].Value, NodeMap);

				FString Chain = FormatCompactChain(FinalTarget, NodeMap, 0, Visited);
				if (!Chain.IsEmpty())
				{
					Lines.Add(Chain);
				}
			}
		}
	}

	return FString::Join(Lines, TEXT("\n"));
}

FString FBlueprintTextFormatter::FormatCompactChain(
	const FString& NodeName,
	const TMap<FString, const FExportedNode*>& NodeMap,
	int32 Indent,
	TSet<FString>& Visited)
{
	// Cycle detection
	if (Visited.Contains(NodeName))
	{
		return FString::ChrN(Indent * 2, TEXT(' ')) + TEXT("[continues...]");
	}

	const FExportedNode* const* NodePtr = NodeMap.Find(NodeName);
	if (!NodePtr || !*NodePtr)
	{
		return FString();
	}

	const FExportedNode& Node = **NodePtr;
	Visited.Add(NodeName);

	// Skip Knot (reroute) nodes -- should already be resolved, but just in case
	if (Node.NodeClass == TEXT("K2Node_Knot"))
	{
		for (const FExportedPin& Pin : Node.Pins)
		{
			if (Pin.Category == TEXT("exec") && Pin.Direction == TEXT("Output")
				&& Pin.LinkedTo.Num() > 0)
			{
				auto [FinalTarget, _] = ResolveRerouteChain(
					Pin.LinkedTo[0].Key, Pin.LinkedTo[0].Value, NodeMap);
				return FormatCompactChain(FinalTarget, NodeMap, Indent, Visited);
			}
		}
		return FString();
	}

	FString IndentStr = FString::ChrN(Indent * 2, TEXT(' '));
	FString Title = GetSemanticTitle(Node);

	// For FunctionResult nodes, show return values inline
	if (Node.NodeClass == TEXT("K2Node_FunctionResult"))
	{
		TArray<FString> RetVals;
		for (const FExportedPin& Pin : Node.Pins)
		{
			if (Pin.Category == TEXT("exec") || Pin.Direction != TEXT("Input"))
			{
				continue;
			}
			FString ValStr = Pin.Name;
			if (!Pin.DefaultValue.IsEmpty() && !IsTrivialDefault(Pin.DefaultValue))
			{
				ValStr += TEXT("=") + Pin.DefaultValue;
			}
			else if (Pin.LinkedTo.Num() > 0)
			{
				// Show what feeds this return value using semantic title
				const FString& SourceNodeName = Pin.LinkedTo[0].Key;
				const FExportedNode* const* SourcePtr = NodeMap.Find(SourceNodeName);
				if (SourcePtr && *SourcePtr)
				{
					ValStr += TEXT("=") + GetSemanticTitle(**SourcePtr);
				}
			}
			RetVals.Add(ValStr);
		}
		return IndentStr + TEXT("return(") + FString::Join(RetVals, TEXT(", ")) + TEXT(")");
	}

	// Collect exec output pins with connections
	struct FExecBranch
	{
		FString Label;
		FString TargetNodeName;
	};
	TArray<FExecBranch> ExecBranches;

	for (const FExportedPin& Pin : Node.Pins)
	{
		if (Pin.Category != TEXT("exec") || Pin.Direction != TEXT("Output")
			|| Pin.LinkedTo.Num() == 0)
		{
			continue;
		}

		for (const auto& Link : Pin.LinkedTo)
		{
			auto [FinalTarget, _] = ResolveRerouteChain(Link.Key, Link.Value, NodeMap);

			FString Label = Pin.Name;
			// Normalize standard labels
			if (Label == TEXT("then") || Label == TEXT("execute") || Label == TEXT("OutputPin"))
			{
				Label.Empty();
			}

			ExecBranches.Add({ Label, FinalTarget });
		}
	}

	TArray<FString> Lines;

	if (ExecBranches.Num() == 0)
	{
		// Terminal node (no exec outputs connected)
		Lines.Add(IndentStr + Title);
	}
	else if (ExecBranches.Num() == 1 && ExecBranches[0].Label.IsEmpty())
	{
		// Linear chain -- show node, then continue
		Lines.Add(IndentStr + Title);
		FString NextChain = FormatCompactChain(
			ExecBranches[0].TargetNodeName, NodeMap, Indent, Visited);
		if (!NextChain.IsEmpty())
		{
			Lines.Add(NextChain);
		}
	}
	else
	{
		// Branching -- show tree structure
		Lines.Add(IndentStr + Title + TEXT(":"));

		for (int32 i = 0; i < ExecBranches.Num(); i++)
		{
			bool bIsLast = (i == ExecBranches.Num() - 1);
			FString Prefix = bIsLast ? TEXT("\u2514") : TEXT("\u251C");
			FString Connector = bIsLast ? TEXT("  ") : TEXT("\u2502 ");

			FString BranchLabel = ExecBranches[i].Label.IsEmpty()
				? FString::Printf(TEXT("Then %d"), i)
				: ExecBranches[i].Label;

			FString BranchChain = FormatCompactChain(
				ExecBranches[i].TargetNodeName, NodeMap, Indent + 1, Visited);

			if (BranchChain.IsEmpty())
			{
				Lines.Add(IndentStr + Prefix + TEXT(" [") + BranchLabel + TEXT("]: (end)"));
			}
			else
			{
				// Check if the chain is a single line (can inline)
				if (!BranchChain.Contains(TEXT("\n")))
				{
					Lines.Add(IndentStr + Prefix + TEXT(" [") + BranchLabel + TEXT("]: ")
						+ BranchChain.TrimStart());
				}
				else
				{
					Lines.Add(IndentStr + Prefix + TEXT(" [") + BranchLabel + TEXT("]:"));
					Lines.Add(BranchChain);
				}
			}
		}
	}

	return FString::Join(Lines, TEXT("\n"));
}
