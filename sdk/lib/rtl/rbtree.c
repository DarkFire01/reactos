/*
 * PROJECT:     ReactOS Runtime Library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Red black trees
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The caller does the searching and says where a node belongs, so this only
 * has to link the node in and put the colours right again. RTL_BALANCED_NODE
 * keeps its parent pointer and its colour in one field, since a node is always
 * at least four byte aligned and the low bits are free.
 */

#include <rtl.h>

#define NDEBUG
#include <debug.h>

/*
 * This library is built against the user mode headers, where neither the node
 * nor the tree is declared. The layouts are the published ones, kept here
 * until a header this can reach carries them.
 */
#ifdef _MSC_VER
 #pragma warning(push)
 #pragma warning(disable:4214) /* Bit fields of other types than int */
#endif
typedef struct _RTL_BALANCED_NODE
{
    _ANONYMOUS_UNION union
    {
        struct _RTL_BALANCED_NODE *Children[2];
        _ANONYMOUS_STRUCT struct
        {
            struct _RTL_BALANCED_NODE *Left;
            struct _RTL_BALANCED_NODE *Right;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME;
    _ANONYMOUS_UNION union
    {
        UCHAR Red : 1;
        UCHAR Balance : 2;
        ULONG_PTR ParentValue;
    } DUMMYUNIONNAME2;
} RTL_BALANCED_NODE, *PRTL_BALANCED_NODE;
#ifdef _MSC_VER
 #pragma warning(pop)
#endif

#define RTL_BALANCED_NODE_RESERVED_PARENT_MASK 3

#define RTL_BALANCED_NODE_GET_PARENT_POINTER(Node) \
    ((PRTL_BALANCED_NODE)((Node)->ParentValue & ~RTL_BALANCED_NODE_RESERVED_PARENT_MASK))

typedef struct _RTL_RB_TREE
{
    PRTL_BALANCED_NODE Root;
    PRTL_BALANCED_NODE Min;
} RTL_RB_TREE, *PRTL_RB_TREE;

/* PRIVATE FUNCTIONS **********************************************************/

static
PRTL_BALANCED_NODE
RtlpRbParent(
    _In_ PRTL_BALANCED_NODE Node)
{
    return RTL_BALANCED_NODE_GET_PARENT_POINTER(Node);
}

static
VOID
RtlpRbSetParent(
    _Inout_ PRTL_BALANCED_NODE Node,
    _In_opt_ PRTL_BALANCED_NODE Parent)
{
    Node->ParentValue = (ULONG_PTR)Parent | (Node->ParentValue & RTL_BALANCED_NODE_RESERVED_PARENT_MASK);
}

static
BOOLEAN
RtlpRbIsRed(
    _In_opt_ PRTL_BALANCED_NODE Node)
{
    /* A leaf that is not there is black */
    return ((Node != NULL) && (Node->Red != 0));
}

static
VOID
RtlpRbSetRed(
    _Inout_ PRTL_BALANCED_NODE Node,
    _In_ BOOLEAN Red)
{
    if (Red)
        Node->ParentValue |= 1;
    else
        Node->ParentValue &= ~(ULONG_PTR)1;
}

/**
 * @brief
 * Puts a node where its child is, and takes the child's place.
 *
 * @param[in] Right
 * TRUE to rotate left, which lifts the right child, and FALSE for the mirror
 * of that.
 */
static
VOID
RtlpRbRotate(
    _Inout_ PRTL_RB_TREE Tree,
    _Inout_ PRTL_BALANCED_NODE Node,
    _In_ BOOLEAN Right)
{
    PRTL_BALANCED_NODE Lifted = Node->Children[Right ? 1 : 0];
    PRTL_BALANCED_NODE Parent;

    Node->Children[Right ? 1 : 0] = Lifted->Children[Right ? 0 : 1];
    if (Lifted->Children[Right ? 0 : 1] != NULL)
        RtlpRbSetParent(Lifted->Children[Right ? 0 : 1], Node);

    Parent = RtlpRbParent(Node);
    RtlpRbSetParent(Lifted, Parent);

    if (Parent == NULL)
        Tree->Root = Lifted;
    else if (Parent->Left == Node)
        Parent->Left = Lifted;
    else
        Parent->Right = Lifted;

    Lifted->Children[Right ? 0 : 1] = Node;
    RtlpRbSetParent(Node, Lifted);
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Links a node into a tree below a parent the caller already found.
 *
 * @param[in] Parent
 * The node the new one hangs from, or NULL for the first node of a tree.
 *
 * @param[in] Right
 * Which side of the parent the new node goes on.
 */
VOID
NTAPI
RtlRbInsertNodeEx(
    _Inout_ PRTL_RB_TREE Tree,
    _In_opt_ PRTL_BALANCED_NODE Parent,
    _In_ BOOLEAN Right,
    _Out_ PRTL_BALANCED_NODE Node)
{
    PRTL_BALANCED_NODE Grandparent, Uncle;

    Node->Left = NULL;
    Node->Right = NULL;
    Node->ParentValue = (ULONG_PTR)Parent;
    RtlpRbSetRed(Node, TRUE);

    if (Parent == NULL)
    {
        Tree->Root = Node;
        Tree->Min = Node;
        RtlpRbSetRed(Node, FALSE);
        return;
    }

    Parent->Children[Right ? 1 : 0] = Node;

    /* The smallest node is the one nothing sits to the left of */
    if (!Right && (Tree->Min == Parent))
        Tree->Min = Node;

    /* Two reds in a row is the only thing an insert can break */
    while (RtlpRbIsRed(RtlpRbParent(Node)))
    {
        Parent = RtlpRbParent(Node);
        Grandparent = RtlpRbParent(Parent);
        if (Grandparent == NULL)
            break;

        if (Parent == Grandparent->Left)
        {
            Uncle = Grandparent->Right;

            if (RtlpRbIsRed(Uncle))
            {
                RtlpRbSetRed(Parent, FALSE);
                RtlpRbSetRed(Uncle, FALSE);
                RtlpRbSetRed(Grandparent, TRUE);
                Node = Grandparent;
                continue;
            }

            if (Node == Parent->Right)
            {
                Node = Parent;
                RtlpRbRotate(Tree, Node, TRUE);
                Parent = RtlpRbParent(Node);
                Grandparent = RtlpRbParent(Parent);
            }

            RtlpRbSetRed(Parent, FALSE);
            RtlpRbSetRed(Grandparent, TRUE);
            RtlpRbRotate(Tree, Grandparent, FALSE);
        }
        else
        {
            Uncle = Grandparent->Left;

            if (RtlpRbIsRed(Uncle))
            {
                RtlpRbSetRed(Parent, FALSE);
                RtlpRbSetRed(Uncle, FALSE);
                RtlpRbSetRed(Grandparent, TRUE);
                Node = Grandparent;
                continue;
            }

            if (Node == Parent->Left)
            {
                Node = Parent;
                RtlpRbRotate(Tree, Node, FALSE);
                Parent = RtlpRbParent(Node);
                Grandparent = RtlpRbParent(Parent);
            }

            RtlpRbSetRed(Parent, FALSE);
            RtlpRbSetRed(Grandparent, TRUE);
            RtlpRbRotate(Tree, Grandparent, TRUE);
        }
    }

    RtlpRbSetRed(Tree->Root, FALSE);
}

/**
 * @brief
 * Takes a node out of a tree.
 */
VOID
NTAPI
RtlRbRemoveNode(
    _Inout_ PRTL_RB_TREE Tree,
    _Inout_ PRTL_BALANCED_NODE Node)
{
    PRTL_BALANCED_NODE Child, Parent, Sibling, Successor;
    BOOLEAN WasRed;

    if (Tree->Min == Node)
    {
        /* Whatever follows it is the new smallest */
        if (Node->Right != NULL)
        {
            Successor = Node->Right;
            while (Successor->Left != NULL)
                Successor = Successor->Left;

            Tree->Min = Successor;
        }
        else
        {
            Tree->Min = RtlpRbParent(Node);
        }
    }

    if ((Node->Left != NULL) && (Node->Right != NULL))
    {
        /*
         * Two children, so the node that follows it takes its place. Only the
         * contents of the link fields move, which keeps the caller's pointers
         * to other nodes good.
         */
        Successor = Node->Right;
        while (Successor->Left != NULL)
            Successor = Successor->Left;

        WasRed = (BOOLEAN)RtlpRbIsRed(Successor);
        Child = Successor->Right;
        Parent = RtlpRbParent(Successor);

        if (Parent == Node)
        {
            Parent = Successor;
        }
        else
        {
            if (Child != NULL)
                RtlpRbSetParent(Child, Parent);

            Parent->Left = Child;
            Successor->Right = Node->Right;
            RtlpRbSetParent(Node->Right, Successor);
        }

        Successor->Left = Node->Left;
        RtlpRbSetParent(Node->Left, Successor);
        RtlpRbSetParent(Successor, RtlpRbParent(Node));
        RtlpRbSetRed(Successor, (BOOLEAN)RtlpRbIsRed(Node));

        if (RtlpRbParent(Node) == NULL)
            Tree->Root = Successor;
        else if (RtlpRbParent(Node)->Left == Node)
            RtlpRbParent(Node)->Left = Successor;
        else
            RtlpRbParent(Node)->Right = Successor;
    }
    else
    {
        WasRed = (BOOLEAN)RtlpRbIsRed(Node);
        Child = (Node->Left != NULL) ? Node->Left : Node->Right;
        Parent = RtlpRbParent(Node);

        if (Child != NULL)
            RtlpRbSetParent(Child, Parent);

        if (Parent == NULL)
            Tree->Root = Child;
        else if (Parent->Left == Node)
            Parent->Left = Child;
        else
            Parent->Right = Child;
    }

    if (WasRed)
        return;

    /* Taking a black node out leaves one side of the tree one short */
    while ((Child != Tree->Root) && !RtlpRbIsRed(Child))
    {
        if (Parent == NULL)
            break;

        if (Child == Parent->Left)
        {
            Sibling = Parent->Right;

            if (RtlpRbIsRed(Sibling))
            {
                RtlpRbSetRed(Sibling, FALSE);
                RtlpRbSetRed(Parent, TRUE);
                RtlpRbRotate(Tree, Parent, TRUE);
                Sibling = Parent->Right;
            }

            if (Sibling == NULL)
                break;

            if (!RtlpRbIsRed(Sibling->Left) && !RtlpRbIsRed(Sibling->Right))
            {
                RtlpRbSetRed(Sibling, TRUE);
                Child = Parent;
                Parent = RtlpRbParent(Child);
                continue;
            }

            if (!RtlpRbIsRed(Sibling->Right))
            {
                RtlpRbSetRed(Sibling->Left, FALSE);
                RtlpRbSetRed(Sibling, TRUE);
                RtlpRbRotate(Tree, Sibling, FALSE);
                Sibling = Parent->Right;
            }

            RtlpRbSetRed(Sibling, (BOOLEAN)RtlpRbIsRed(Parent));
            RtlpRbSetRed(Parent, FALSE);
            if (Sibling->Right != NULL)
                RtlpRbSetRed(Sibling->Right, FALSE);

            RtlpRbRotate(Tree, Parent, TRUE);
            Child = Tree->Root;
            break;
        }
        else
        {
            Sibling = Parent->Left;

            if (RtlpRbIsRed(Sibling))
            {
                RtlpRbSetRed(Sibling, FALSE);
                RtlpRbSetRed(Parent, TRUE);
                RtlpRbRotate(Tree, Parent, FALSE);
                Sibling = Parent->Left;
            }

            if (Sibling == NULL)
                break;

            if (!RtlpRbIsRed(Sibling->Left) && !RtlpRbIsRed(Sibling->Right))
            {
                RtlpRbSetRed(Sibling, TRUE);
                Child = Parent;
                Parent = RtlpRbParent(Child);
                continue;
            }

            if (!RtlpRbIsRed(Sibling->Left))
            {
                RtlpRbSetRed(Sibling->Right, FALSE);
                RtlpRbSetRed(Sibling, TRUE);
                RtlpRbRotate(Tree, Sibling, TRUE);
                Sibling = Parent->Left;
            }

            RtlpRbSetRed(Sibling, (BOOLEAN)RtlpRbIsRed(Parent));
            RtlpRbSetRed(Parent, FALSE);
            if (Sibling->Left != NULL)
                RtlpRbSetRed(Sibling->Left, FALSE);

            RtlpRbRotate(Tree, Parent, FALSE);
            Child = Tree->Root;
            break;
        }
    }

    if (Child != NULL)
        RtlpRbSetRed(Child, FALSE);
}

/* EOF */
