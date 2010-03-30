#include "misc.hh"
#include "eval.hh"
#include "globals.hh"
#include "store-api.hh"
#include "util.hh"
#include "archive.hh"
#include "expr-to-xml.hh"
#include "nixexpr-ast.hh"
#include "parser.hh"
#include "names.hh"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>


namespace nix {


/*************************************************************
 * Miscellaneous
 *************************************************************/


/* Load and evaluate an expression from path specified by the
   argument. */ 
static void prim_import(EvalState & state, Value * * args, Value & v)
{
    PathSet context;
    Path path = state.coerceToPath(*args[0], context);

    for (PathSet::iterator i = context.begin(); i != context.end(); ++i) {
        assert(isStorePath(*i));
        if (!store->isValidPath(*i))
            throw EvalError(format("cannot import `%1%', since path `%2%' is not valid")
                % path % *i);
        if (isDerivation(*i))
            store->buildDerivations(singleton<PathSet>(*i));
    }

    state.evalFile(path, v);
}


#if 0
/* Determine whether the argument is the null value. */
static void prim_isNull(EvalState & state, Value * * args, Value & v)
{
    return makeBool(matchNull(evalExpr(state, args[0])));
}


/* Determine whether the argument is a function. */
static void prim_isFunction(EvalState & state, Value * * args, Value & v)
{
    Expr e = evalExpr(state, args[0]);
    Pattern pat;
    ATerm body, pos;
    return makeBool(matchFunction(e, pat, body, pos));
}

/* Determine whether the argument is an Int. */
static void prim_isInt(EvalState & state, Value * * args, Value & v)
{
    int i;
    return makeBool(matchInt(evalExpr(state, args[0]), i));
}

/* Determine whether the argument is an String. */
static void prim_isString(EvalState & state, Value * * args, Value & v)
{
    string s;
    PathSet l;
    return makeBool(matchStr(evalExpr(state, args[0]), s, l));
}

/* Determine whether the argument is an Bool. */
static void prim_isBool(EvalState & state, Value * * args, Value & v)
{
    ATermBool b;
    return makeBool(matchBool(evalExpr(state, args[0]), b));
}

static void prim_genericClosure(EvalState & state, Value * * args, Value & v)
{
    startNest(nest, lvlDebug, "finding dependencies");

    Expr attrs = evalExpr(state, args[0]);

    /* Get the start set. */
    Expr startSet = queryAttr(attrs, "startSet");
    if (!startSet) throw EvalError("attribute `startSet' required");
    ATermList startSet2 = evalList(state, startSet);

    set<Expr> workSet; // !!! gc roots
    for (ATermIterator i(startSet2); i; ++i) workSet.insert(*i);

    /* Get the operator. */
    Expr op = queryAttr(attrs, "operator");
    if (!op) throw EvalError("attribute `operator' required");
    
    /* Construct the closure by applying the operator to element of
       `workSet', adding the result to `workSet', continuing until
       no new elements are found. */
    ATermList res = ATempty;
    set<Expr> doneKeys; // !!! gc roots
    while (!workSet.empty()) {
	Expr e = *(workSet.begin());
	workSet.erase(e);

        e = strictEvalExpr(state, e);

        Expr key = queryAttr(e, "key");
        if (!key) throw EvalError("attribute `key' required");

	if (doneKeys.find(key) != doneKeys.end()) continue;
        doneKeys.insert(key);
        res = ATinsert(res, e);
        
        /* Call the `operator' function with `e' as argument. */
        ATermList res = evalList(state, makeCall(op, e));

        /* Try to find the dependencies relative to the `path'. */
        for (ATermIterator i(res); i; ++i)
            workSet.insert(evalExpr(state, *i));
    }

    return makeList(res);
}


static void prim_abort(EvalState & state, Value * * args, Value & v)
{
    PathSet context;
    throw Abort(format("evaluation aborted with the following error message: `%1%'") %
        evalString(state, args[0], context));
}


static void prim_throw(EvalState & state, Value * * args, Value & v)
{
    PathSet context;
    throw ThrownError(format("user-thrown exception: %1%") %
        evalString(state, args[0], context));
}


static void prim_addErrorContext(EvalState & state, Value * * args, Value & v)
{
    PathSet context;
    try {
        return evalExpr(state, args[1]);
    } catch (Error & e) {
        e.addPrefix(format("%1%\n") %
            evalString(state, args[0], context));
        throw;
    }
}

/* Try evaluating the argument. Success => {success=true; value=something;}, 
 * else => {success=false; value=false;} */
static void prim_tryEval(EvalState & state, Value * * args, Value & v)
{
    ATermMap res = ATermMap();
    try {
        Expr val = evalExpr(state, args[0]);
        res.set(toATerm("value"), makeAttrRHS(val, makeNoPos()));
        res.set(toATerm("success"), makeAttrRHS(eTrue, makeNoPos()));
    } catch (AssertionError & e) {
        printMsg(lvlDebug, format("tryEval caught an error: %1%: %2%") % e.prefix() % e.msg());
        res.set(toATerm("value"), makeAttrRHS(eFalse, makeNoPos()));
        res.set(toATerm("success"), makeAttrRHS(eFalse, makeNoPos()));
    }
    return makeAttrs(res);
}


/* Return an environment variable.  Use with care. */
static void prim_getEnv(EvalState & state, Value * * args, Value & v)
{
    string name = evalStringNoCtx(state, args[0]);
    return makeStr(getEnv(name));
}


/* Evaluate the first expression, and print its abstract syntax tree
   on standard error.  Then return the second expression.  Useful for
   debugging.
 */
static void prim_trace(EvalState & state, Value * * args, Value & v)
{
    Expr e = evalExpr(state, args[0]);
    string s;
    PathSet context;
    if (matchStr(e, s, context))
        printMsg(lvlError, format("trace: %1%") % s);
    else
        printMsg(lvlError, format("trace: %1%") % e);
    return evalExpr(state, args[1]);
}


/*************************************************************
 * Derivations
 *************************************************************/


static bool isFixedOutput(const Derivation & drv)
{
    return drv.outputs.size() == 1 &&
        drv.outputs.begin()->first == "out" &&
        drv.outputs.begin()->second.hash != "";
}


/* Returns the hash of a derivation modulo fixed-output
   subderivations.  A fixed-output derivation is a derivation with one
   output (`out') for which an expected hash and hash algorithm are
   specified (using the `outputHash' and `outputHashAlgo'
   attributes).  We don't want changes to such derivations to
   propagate upwards through the dependency graph, changing output
   paths everywhere.

   For instance, if we change the url in a call to the `fetchurl'
   function, we do not want to rebuild everything depending on it
   (after all, (the hash of) the file being downloaded is unchanged).
   So the *output paths* should not change.  On the other hand, the
   *derivation paths* should change to reflect the new dependency
   graph.

   That's what this function does: it returns a hash which is just the
   hash of the derivation ATerm, except that any input derivation
   paths have been replaced by the result of a recursive call to this
   function, and that for fixed-output derivations we return a hash of
   its output path. */
static Hash hashDerivationModulo(EvalState & state, Derivation drv)
{
    /* Return a fixed hash for fixed-output derivations. */
    if (isFixedOutput(drv)) {
        DerivationOutputs::const_iterator i = drv.outputs.begin();
        return hashString(htSHA256, "fixed:out:"
            + i->second.hashAlgo + ":"
            + i->second.hash + ":"
            + i->second.path);
    }

    /* For other derivations, replace the inputs paths with recursive
       calls to this function.*/
    DerivationInputs inputs2;
    foreach (DerivationInputs::const_iterator, i, drv.inputDrvs) {
        Hash h = state.drvHashes[i->first];
        if (h.type == htUnknown) {
            Derivation drv2 = derivationFromPath(i->first);
            h = hashDerivationModulo(state, drv2);
            state.drvHashes[i->first] = h;
        }
        inputs2[printHash(h)] = i->second;
    }
    drv.inputDrvs = inputs2;
    
    return hashTerm(unparseDerivation(drv));
}


/* Construct (as a unobservable side effect) a Nix derivation
   expression that performs the derivation described by the argument
   set.  Returns the original set extended with the following
   attributes: `outPath' containing the primary output path of the
   derivation; `drvPath' containing the path of the Nix expression;
   and `type' set to `derivation' to indicate that this is a
   derivation. */
static void prim_derivationStrict(EvalState & state, Value * * args, Value & v)
{
    startNest(nest, lvlVomit, "evaluating derivation");

    ATermMap attrs;
    queryAllAttrs(evalExpr(state, args[0]), attrs, true);

    /* Figure out the name already (for stack backtraces). */
    ATerm posDrvName;
    Expr eDrvName = attrs.get(toATerm("name"));
    if (!eDrvName)
        throw EvalError("required attribute `name' missing");
    if (!matchAttrRHS(eDrvName, eDrvName, posDrvName)) abort();
    string drvName;
    try {        
        drvName = evalStringNoCtx(state, eDrvName);
    } catch (Error & e) {
        e.addPrefix(format("while evaluating the derivation attribute `name' at %1%:\n")
            % showPos(posDrvName));
        throw;
    }

    /* Build the derivation expression by processing the attributes. */
    Derivation drv;
    
    PathSet context;

    string outputHash, outputHashAlgo;
    bool outputHashRecursive = false;

    for (ATermMap::const_iterator i = attrs.begin(); i != attrs.end(); ++i) {
        string key = aterm2String(i->key);
        ATerm value;
        Expr pos;
        ATerm rhs = i->value;
        if (!matchAttrRHS(rhs, value, pos)) abort();
        startNest(nest, lvlVomit, format("processing attribute `%1%'") % key);

        try {

            /* The `args' attribute is special: it supplies the
               command-line arguments to the builder. */
            if (key == "args") {
                ATermList es;
                value = evalExpr(state, value);
                if (!matchList(value, es)) {
                    static bool haveWarned = false;
                    warnOnce(haveWarned, "the `args' attribute should evaluate to a list");
                    es = flattenList(state, value);
                }
                for (ATermIterator i(es); i; ++i) {
                    string s = coerceToString(state, *i, context, true);
                    drv.args.push_back(s);
                }
            }

            /* All other attributes are passed to the builder through
               the environment. */
            else {
                string s = coerceToString(state, value, context, true);
                drv.env[key] = s;
                if (key == "builder") drv.builder = s;
                else if (key == "system") drv.platform = s;
                else if (key == "name") drvName = s;
                else if (key == "outputHash") outputHash = s;
                else if (key == "outputHashAlgo") outputHashAlgo = s;
                else if (key == "outputHashMode") {
                    if (s == "recursive") outputHashRecursive = true; 
                    else if (s == "flat") outputHashRecursive = false;
                    else throw EvalError(format("invalid value `%1%' for `outputHashMode' attribute") % s);
                }
            }

        } catch (Error & e) {
            e.addPrefix(format("while evaluating the derivation attribute `%1%' at %2%:\n")
                % key % showPos(pos));
            e.addPrefix(format("while instantiating the derivation named `%1%' at %2%:\n")
                % drvName % showPos(posDrvName));
            throw;
        }

    }
    
    /* Everything in the context of the strings in the derivation
       attributes should be added as dependencies of the resulting
       derivation. */
    foreach (PathSet::iterator, i, context) {
        Path path = *i;
        
        /* Paths marked with `=' denote that the path of a derivation
           is explicitly passed to the builder.  Since that allows the
           builder to gain access to every path in the dependency
           graph of the derivation (including all outputs), all paths
           in the graph must be added to this derivation's list of
           inputs to ensure that they are available when the builder
           runs. */
        if (path.at(0) == '=') {
            path = string(path, 1);
            PathSet refs; computeFSClosure(path, refs);
            foreach (PathSet::iterator, j, refs) {
                drv.inputSrcs.insert(*j);
                if (isDerivation(*j))
                    drv.inputDrvs[*j] = singleton<StringSet>("out");
            }
        }

        /* See prim_unsafeDiscardOutputDependency. */
        bool useDrvAsSrc = false;
        if (path.at(0) == '~') {
            path = string(path, 1);
            useDrvAsSrc = true;
        }

        assert(isStorePath(path));

        debug(format("derivation uses `%1%'") % path);
        if (!useDrvAsSrc && isDerivation(path))
            drv.inputDrvs[path] = singleton<StringSet>("out");
        else
            drv.inputSrcs.insert(path);
    }
            
    /* Do we have all required attributes? */
    if (drv.builder == "")
        throw EvalError("required attribute `builder' missing");
    if (drv.platform == "")
        throw EvalError("required attribute `system' missing");

    /* If an output hash was given, check it. */
    Path outPath;
    if (outputHash == "")
        outputHashAlgo = "";
    else {
        HashType ht = parseHashType(outputHashAlgo);
        if (ht == htUnknown)
            throw EvalError(format("unknown hash algorithm `%1%'") % outputHashAlgo);
        Hash h(ht);
        if (outputHash.size() == h.hashSize * 2)
            /* hexadecimal representation */
            h = parseHash(ht, outputHash);
        else if (outputHash.size() == hashLength32(h))
            /* base-32 representation */
            h = parseHash32(ht, outputHash);
        else
            throw Error(format("hash `%1%' has wrong length for hash type `%2%'")
                % outputHash % outputHashAlgo);
        string s = outputHash;
        outputHash = printHash(h);
        outPath = makeFixedOutputPath(outputHashRecursive, ht, h, drvName);
        if (outputHashRecursive) outputHashAlgo = "r:" + outputHashAlgo;
    }

    /* Check whether the derivation name is valid. */
    checkStoreName(drvName);
    if (isDerivation(drvName))
        throw EvalError(format("derivation names are not allowed to end in `%1%'")
            % drvExtension);

    /* Construct the "masked" derivation store expression, which is
       the final one except that in the list of outputs, the output
       paths are empty, and the corresponding environment variables
       have an empty value.  This ensures that changes in the set of
       output names do get reflected in the hash. */
    drv.env["out"] = "";
    drv.outputs["out"] = DerivationOutput("", outputHashAlgo, outputHash);
        
    /* Use the masked derivation expression to compute the output
       path. */
    if (outPath == "")
        outPath = makeStorePath("output:out", hashDerivationModulo(state, drv), drvName);

    /* Construct the final derivation store expression. */
    drv.env["out"] = outPath;
    drv.outputs["out"] =
        DerivationOutput(outPath, outputHashAlgo, outputHash);

    /* Write the resulting term into the Nix store directory. */
    Path drvPath = writeDerivation(drv, drvName);

    printMsg(lvlChatty, format("instantiated `%1%' -> `%2%'")
        % drvName % drvPath);

    /* Optimisation, but required in read-only mode! because in that
       case we don't actually write store expressions, so we can't
       read them later. */
    state.drvHashes[drvPath] = hashDerivationModulo(state, drv);

    /* !!! assumes a single output */
    ATermMap outAttrs(2);
    outAttrs.set(toATerm("outPath"),
        makeAttrRHS(makeStr(outPath, singleton<PathSet>(drvPath)), makeNoPos()));
    outAttrs.set(toATerm("drvPath"),
        makeAttrRHS(makeStr(drvPath, singleton<PathSet>("=" + drvPath)), makeNoPos()));

    return makeAttrs(outAttrs);
}


static void prim_derivationLazy(EvalState & state, Value * * args, Value & v)
{
    Expr eAttrs = evalExpr(state, args[0]);
    ATermMap attrs;    
    queryAllAttrs(eAttrs, attrs, true);

    attrs.set(toATerm("type"),
        makeAttrRHS(makeStr("derivation"), makeNoPos()));

    Expr drvStrict = makeCall(makeVar(toATerm("derivation!")), eAttrs);

    attrs.set(toATerm("outPath"),
        makeAttrRHS(makeSelect(drvStrict, toATerm("outPath")), makeNoPos()));
    attrs.set(toATerm("drvPath"),
        makeAttrRHS(makeSelect(drvStrict, toATerm("drvPath")), makeNoPos()));
    
    return makeAttrs(attrs);
}


/*************************************************************
 * Paths
 *************************************************************/


/* Convert the argument to a path.  !!! obsolete? */
static void prim_toPath(EvalState & state, Value * * args, Value & v)
{
    PathSet context;
    string path = coerceToPath(state, args[0], context);
    return makeStr(canonPath(path), context);
}


/* Allow a valid store path to be used in an expression.  This is
   useful in some generated expressions such as in nix-push, which
   generates a call to a function with an already existing store path
   as argument.  You don't want to use `toPath' here because it copies
   the path to the Nix store, which yields a copy like
   /nix/store/newhash-oldhash-oldname.  In the past, `toPath' had
   special case behaviour for store paths, but that created weird
   corner cases. */
static void prim_storePath(EvalState & state, Value * * args, Value & v)
{
    PathSet context;
    Path path = canonPath(coerceToPath(state, args[0], context));
    if (!isInStore(path))
        throw EvalError(format("path `%1%' is not in the Nix store") % path);
    Path path2 = toStorePath(path);
    if (!store->isValidPath(path2))
        throw EvalError(format("store path `%1%' is not valid") % path2);
    context.insert(path2);
    return makeStr(path, context);
}


static void prim_pathExists(EvalState & state, Value * * args, Value & v)
{
    PathSet context;
    Path path = coerceToPath(state, args[0], context);
    if (!context.empty())
        throw EvalError(format("string `%1%' cannot refer to other paths") % path);
    return makeBool(pathExists(path));
}


/* Return the base name of the given string, i.e., everything
   following the last slash. */
static void prim_baseNameOf(EvalState & state, Value * * args, Value & v)
{
    PathSet context;
    return makeStr(baseNameOf(coerceToString(state, args[0], context)), context);
}


/* Return the directory of the given path, i.e., everything before the
   last slash.  Return either a path or a string depending on the type
   of the argument. */
static void prim_dirOf(EvalState & state, Value * * args, Value & v)
{
    PathSet context;
    Expr e = evalExpr(state, args[0]); ATerm dummy;
    bool isPath = matchPath(e, dummy);
    Path dir = dirOf(coerceToPath(state, e, context));
    return isPath ? makePath(toATerm(dir)) : makeStr(dir, context);
}


/* Return the contents of a file as a string. */
static void prim_readFile(EvalState & state, Value * * args, Value & v)
{
    PathSet context;
    Path path = coerceToPath(state, args[0], context);
    if (!context.empty())
        throw EvalError(format("string `%1%' cannot refer to other paths") % path);
    return makeStr(readFile(path));
}


/*************************************************************
 * Creating files
 *************************************************************/


/* Convert the argument (which can be any Nix expression) to an XML
   representation returned in a string.  Not all Nix expressions can
   be sensibly or completely represented (e.g., functions). */
static void prim_toXML(EvalState & state, Value * * args, Value & v)
{
    std::ostringstream out;
    PathSet context;
    printTermAsXML(strictEvalExpr(state, args[0]), out, context);
    return makeStr(out.str(), context);
}


/* Store a string in the Nix store as a source file that can be used
   as an input by derivations. */
static void prim_toFile(EvalState & state, Value * * args, Value & v)
{
    PathSet context;
    string name = evalStringNoCtx(state, args[0]);
    string contents = evalString(state, args[1], context);

    PathSet refs;

    for (PathSet::iterator i = context.begin(); i != context.end(); ++i) {
        Path path = *i;
        if (path.at(0) == '=') path = string(path, 1);
        if (isDerivation(path))
            throw EvalError(format("in `toFile': the file `%1%' cannot refer to derivation outputs") % name);
        refs.insert(path);
    }
    
    Path storePath = readOnlyMode
        ? computeStorePathForText(name, contents, refs)
        : store->addTextToStore(name, contents, refs);

    /* Note: we don't need to add `context' to the context of the
       result, since `storePath' itself has references to the paths
       used in args[1]. */
    
    return makeStr(storePath, singleton<PathSet>(storePath));
}


struct FilterFromExpr : PathFilter
{
    EvalState & state;
    Expr filter;
    
    FilterFromExpr(EvalState & state, Expr filter)
        : state(state), filter(filter)
    {
    }

    bool operator () (const Path & path)
    {
        struct stat st;
        if (lstat(path.c_str(), &st))
            throw SysError(format("getting attributes of path `%1%'") % path);

        Expr call =
            makeCall(
                makeCall(filter, makeStr(path)),
                makeStr(
                    S_ISREG(st.st_mode) ? "regular" :
                    S_ISDIR(st.st_mode) ? "directory" :
                    S_ISLNK(st.st_mode) ? "symlink" :
                    "unknown" /* not supported, will fail! */
                    ));
                
        return evalBool(state, call);
    }
};


static void prim_filterSource(EvalState & state, Value * * args, Value & v)
{
    PathSet context;
    Path path = coerceToPath(state, args[1], context);
    if (!context.empty())
        throw EvalError(format("string `%1%' cannot refer to other paths") % path);

    FilterFromExpr filter(state, args[0]);

    Path dstPath = readOnlyMode
        ? computeStorePathForPath(path, true, htSHA256, filter).first
        : store->addToStore(path, true, htSHA256, filter);

    return makeStr(dstPath, singleton<PathSet>(dstPath));
}


/*************************************************************
 * Attribute sets
 *************************************************************/


/* Return the names of the attributes in an attribute set as a sorted
   list of strings. */
static void prim_attrNames(EvalState & state, Value * * args, Value & v)
{
    ATermMap attrs;
    queryAllAttrs(evalExpr(state, args[0]), attrs);

    StringSet names;
    for (ATermMap::const_iterator i = attrs.begin(); i != attrs.end(); ++i)
        names.insert(aterm2String(i->key));

    ATermList list = ATempty;
    for (StringSet::const_reverse_iterator i = names.rbegin();
         i != names.rend(); ++i)
        list = ATinsert(list, makeStr(*i, PathSet()));

    return makeList(list);
}


/* Dynamic version of the `.' operator. */
static void prim_getAttr(EvalState & state, Value * * args, Value & v)
{
    string attr = evalStringNoCtx(state, args[0]);
    return evalExpr(state, makeSelect(args[1], toATerm(attr)));
}


/* Dynamic version of the `?' operator. */
static void prim_hasAttr(EvalState & state, Value * * args, Value & v)
{
    string attr = evalStringNoCtx(state, args[0]);
    return evalExpr(state, makeOpHasAttr(args[1], toATerm(attr)));
}


/* Builds an attribute set from a list specifying (name, value)
   pairs.  To be precise, a list [{name = "name1"; value = value1;}
   ... {name = "nameN"; value = valueN;}] is transformed to {name1 =
   value1; ... nameN = valueN;}. */
static void prim_listToAttrs(EvalState & state, Value * * args, Value & v)
{
    try {
        ATermMap res = ATermMap();
        ATermList list;
        list = evalList(state, args[0]);
        for (ATermIterator i(list); i; ++i){
            // *i should now contain a pointer to the list item expression
            ATermList attrs;
            Expr evaledExpr = evalExpr(state, *i);
            if (matchAttrs(evaledExpr, attrs)){
                Expr e = evalExpr(state, makeSelect(evaledExpr, toATerm("name")));
                string attr = evalStringNoCtx(state,e);
                Expr r = makeSelect(evaledExpr, toATerm("value"));
                res.set(toATerm(attr), makeAttrRHS(r, makeNoPos()));
            }
            else
                throw TypeError(format("list element in `listToAttrs' is %s, expected a set { name = \"<name>\"; value = <value>; }")
                    % showType(evaledExpr));
        }
    
        return makeAttrs(res);
    
    } catch (Error & e) {
        e.addPrefix(format("in `listToAttrs':\n"));
        throw;
    }
}


static void prim_removeAttrs(EvalState & state, Value * * args, Value & v)
{
    ATermMap attrs;
    queryAllAttrs(evalExpr(state, args[0]), attrs, true);
    
    ATermList list = evalList(state, args[1]);

    for (ATermIterator i(list); i; ++i)
        /* It's not an error for *i not to exist. */
        attrs.remove(toATerm(evalStringNoCtx(state, *i)));

    return makeAttrs(attrs);
}


/* Determine whether the argument is an attribute set. */
static void prim_isAttrs(EvalState & state, Value * * args, Value & v)
{
    ATermList list;
    return makeBool(matchAttrs(evalExpr(state, args[0]), list));
}


/* Return the right-biased intersection of two attribute sets as1 and
   as2, i.e. a set that contains every attribute from as2 that is also
   a member of as1. */
static void prim_intersectAttrs(EvalState & state, Value * * args, Value & v)
{
    ATermMap as1, as2;
    queryAllAttrs(evalExpr(state, args[0]), as1, true);
    queryAllAttrs(evalExpr(state, args[1]), as2, true);

    ATermMap res;
    foreach (ATermMap::const_iterator, i, as2)
        if (as1[i->key]) res.set(i->key, i->value);

    return makeAttrs(res);
}


static void attrsInPattern(ATermMap & map, Pattern pat)
{
    ATerm name;
    ATermList formals;
    ATermBool ellipsis;
    if (matchAttrsPat(pat, formals, ellipsis, name)) { 
        for (ATermIterator i(formals); i; ++i) {
            ATerm def;
            if (!matchFormal(*i, name, def)) abort();
            map.set(name, makeAttrRHS(makeBool(def != constNoDefaultValue), makeNoPos()));
        }
    }
}


/* Return a set containing the names of the formal arguments expected
   by the function `f'.  The value of each attribute is a Boolean
   denoting whether has a default value.  For instance,

      functionArgs ({ x, y ? 123}: ...)
   => { x = false; y = true; }

   "Formal argument" here refers to the attributes pattern-matched by
   the function.  Plain lambdas are not included, e.g.

      functionArgs (x: ...)
   => { }
*/
static void prim_functionArgs(EvalState & state, Value * * args, Value & v)
{
    Expr f = evalExpr(state, args[0]);
    ATerm pat, body, pos;
    if (!matchFunction(f, pat, body, pos))
        throw TypeError("`functionArgs' required a function");
    
    ATermMap as;
    attrsInPattern(as, pat);

    return makeAttrs(as);
}


/*************************************************************
 * Lists
 *************************************************************/


/* Determine whether the argument is a list. */
static void prim_isList(EvalState & state, Value * * args, Value & v)
{
    ATermList list;
    return makeBool(matchList(evalExpr(state, args[0]), list));
}
#endif


/* Return the first element of a list. */
static void prim_head(EvalState & state, Value * * args, Value & v)
{
    state.forceList(*args[0]);
    if (args[0]->list.length == 0)
        throw Error("`head' called on an empty list");
    state.forceValue(args[0]->list.elems[0]);
    v = args[0]->list.elems[0];
}


/* Return a list consisting of everything but the the first element of
   a list. */
static void prim_tail(EvalState & state, Value * * args, Value & v)
{
    state.forceList(*args[0]);
    if (args[0]->list.length == 0)
        throw Error("`tail' called on an empty list");
    state.mkList(v, args[0]->list.length - 1);
    for (unsigned int n = 0; n < v.list.length; ++n)
        v.list.elems[n] = args[0]->list.elems[n + 1];
}


/* Apply a function to every element of a list. */
static void prim_map(EvalState & state, Value * * args, Value & v)
{
    state.forceFunction(*args[0]);
    state.forceList(*args[1]);

    state.mkList(v, args[1]->list.length);

    for (unsigned int n = 0; n < v.list.length; ++n) {
        v.list.elems[n].type = tApp;
        v.list.elems[n].app.left = args[0];
        v.list.elems[n].app.right = &args[1]->list.elems[n];
    }
}


#if 0
/* Return the length of a list.  This is an O(1) time operation. */
static void prim_length(EvalState & state, Value * * args, Value & v)
{
    ATermList list = evalList(state, args[0]);
    return makeInt(ATgetLength(list));
}
#endif


/*************************************************************
 * Integer arithmetic
 *************************************************************/


static void prim_add(EvalState & state, Value * * args, Value & v)
{
    mkInt(v, state.forceInt(*args[0]) + state.forceInt(*args[1]));
}


#if 0
static void prim_sub(EvalState & state, Value * * args, Value & v)
{
    int i1 = evalInt(state, args[0]);
    int i2 = evalInt(state, args[1]);
    return makeInt(i1 - i2);
}


static void prim_mul(EvalState & state, Value * * args, Value & v)
{
    int i1 = evalInt(state, args[0]);
    int i2 = evalInt(state, args[1]);
    return makeInt(i1 * i2);
}


static void prim_div(EvalState & state, Value * * args, Value & v)
{
    int i1 = evalInt(state, args[0]);
    int i2 = evalInt(state, args[1]);
    if (i2 == 0) throw EvalError("division by zero");
    return makeInt(i1 / i2);
}
#endif


static void prim_lessThan(EvalState & state, Value * * args, Value & v)
{
    mkBool(v, state.forceInt(*args[0]) < state.forceInt(*args[1]));
}


/*************************************************************
 * String manipulation
 *************************************************************/


/* Convert the argument to a string.  Paths are *not* copied to the
   store, so `toString /foo/bar' yields `"/foo/bar"', not
   `"/nix/store/whatever..."'. */
static void prim_toString(EvalState & state, Value * * args, Value & v)
{
    PathSet context;
    string s = state.coerceToString(*args[0], context, true, false);
    mkString(v, strdup(s.c_str())); // !!! context
}


#if 0
/* `substring start len str' returns the substring of `str' starting
   at character position `min(start, stringLength str)' inclusive and
   ending at `min(start + len, stringLength str)'.  `start' must be
   non-negative. */
static void prim_substring(EvalState & state, Value * * args, Value & v)
{
    int start = evalInt(state, args[0]);
    int len = evalInt(state, args[1]);
    PathSet context;
    string s = coerceToString(state, args[2], context);

    if (start < 0) throw EvalError("negative start position in `substring'");

    return makeStr(string(s, start, len), context);
}


static void prim_stringLength(EvalState & state, Value * * args, Value & v)
{
    PathSet context;
    string s = coerceToString(state, args[0], context);
    return makeInt(s.size());
}


static void prim_unsafeDiscardStringContext(EvalState & state, Value * * args, Value & v)
{
    PathSet context;
    string s = coerceToString(state, args[0], context);
    return makeStr(s, PathSet());
}


/* Sometimes we want to pass a derivation path (i.e. pkg.drvPath) to a
   builder without causing the derivation to be built (for instance,
   in the derivation that builds NARs in nix-push, when doing
   source-only deployment).  This primop marks the string context so
   that builtins.derivation adds the path to drv.inputSrcs rather than
   drv.inputDrvs. */
static void prim_unsafeDiscardOutputDependency(EvalState & state, Value * * args, Value & v)
{
    PathSet context;
    string s = coerceToString(state, args[0], context);

    PathSet context2;
    foreach (PathSet::iterator, i, context) {
        Path p = *i;
        if (p.at(0) == '=') p = "~" + string(p, 1);
        context2.insert(p);
    }
    
    return makeStr(s, context2);
}


/* Expression serialization/deserialization */


static void prim_exprToString(EvalState & state, Value * * args, Value & v)
{
    /* !!! this disregards context */
    return makeStr(atPrint(evalExpr(state, args[0])));
}


static void prim_stringToExpr(EvalState & state, Value * * args, Value & v)
{
    /* !!! this can introduce arbitrary garbage terms in the
       evaluator! */;
    string s;
    PathSet l;
    if (!matchStr(evalExpr(state, args[0]), s, l))
        throw EvalError("stringToExpr needs string argument!");
    return ATreadFromString(s.c_str());
}


/*************************************************************
 * Versions
 *************************************************************/


static void prim_parseDrvName(EvalState & state, Value * * args, Value & v)
{
    string name = evalStringNoCtx(state, args[0]);
    DrvName parsed(name);
    ATermMap attrs(2);
    attrs.set(toATerm("name"), makeAttrRHS(makeStr(parsed.name), makeNoPos()));
    attrs.set(toATerm("version"), makeAttrRHS(makeStr(parsed.version), makeNoPos()));
    return makeAttrs(attrs);
}


static void prim_compareVersions(EvalState & state, Value * * args, Value & v)
{
    string version1 = evalStringNoCtx(state, args[0]);
    string version2 = evalStringNoCtx(state, args[1]);
    int d = compareVersions(version1, version2);
    return makeInt(d);
}
#endif


/*************************************************************
 * Primop registration
 *************************************************************/


void EvalState::createBaseEnv()
{
    baseEnv.up = 0;

    {   Value & v = baseEnv.bindings[toATerm("builtins")];
        v.type = tAttrs;
        v.attrs = new Bindings;
    }

    /* Add global constants such as `true' to the base environment. */
    Value v;

    mkBool(v, true);
    addConstant("true", v);
    
    mkBool(v, false);
    addConstant("false", v);
    
    v.type = tNull;
    addConstant("null", v);

    mkInt(v, time(0));
    addConstant("__currentTime", v);

    mkString(v, strdup(thisSystem.c_str()));
    addConstant("__currentSystem", v);

    // Miscellaneous
    addPrimOp("import", 1, prim_import);
#if 0
    addPrimOp("isNull", 1, prim_isNull);
    addPrimOp("__isFunction", 1, prim_isFunction);
    addPrimOp("__isString", 1, prim_isString);
    addPrimOp("__isInt", 1, prim_isInt);
    addPrimOp("__isBool", 1, prim_isBool);
    addPrimOp("__genericClosure", 1, prim_genericClosure);
    addPrimOp("abort", 1, prim_abort);
    addPrimOp("throw", 1, prim_throw);
    addPrimOp("__addErrorContext", 2, prim_addErrorContext);
    addPrimOp("__tryEval", 1, prim_tryEval);
    addPrimOp("__getEnv", 1, prim_getEnv);
    addPrimOp("__trace", 2, prim_trace);
    
    // Expr <-> String
    addPrimOp("__exprToString", 1, prim_exprToString);
    addPrimOp("__stringToExpr", 1, prim_stringToExpr);

    // Derivations
    addPrimOp("derivation!", 1, prim_derivationStrict);
    addPrimOp("derivation", 1, prim_derivationLazy);

    // Paths
    addPrimOp("__toPath", 1, prim_toPath);
    addPrimOp("__storePath", 1, prim_storePath);
    addPrimOp("__pathExists", 1, prim_pathExists);
    addPrimOp("baseNameOf", 1, prim_baseNameOf);
    addPrimOp("dirOf", 1, prim_dirOf);
    addPrimOp("__readFile", 1, prim_readFile);

    // Creating files
    addPrimOp("__toXML", 1, prim_toXML);
    addPrimOp("__toFile", 2, prim_toFile);
    addPrimOp("__filterSource", 2, prim_filterSource);

    // Attribute sets
    addPrimOp("__attrNames", 1, prim_attrNames);
    addPrimOp("__getAttr", 2, prim_getAttr);
    addPrimOp("__hasAttr", 2, prim_hasAttr);
    addPrimOp("__isAttrs", 1, prim_isAttrs);
    addPrimOp("removeAttrs", 2, prim_removeAttrs);
    addPrimOp("__listToAttrs", 1, prim_listToAttrs);
    addPrimOp("__intersectAttrs", 2, prim_intersectAttrs);
    addPrimOp("__functionArgs", 1, prim_functionArgs);

    // Lists
    addPrimOp("__isList", 1, prim_isList);
#endif
    addPrimOp("__head", 1, prim_head);
    addPrimOp("__tail", 1, prim_tail);
    addPrimOp("map", 2, prim_map);
#if 0
    addPrimOp("__length", 1, prim_length);
#endif
    
    // Integer arithmetic
    addPrimOp("__add", 2, prim_add);
#if 0
    addPrimOp("__sub", 2, prim_sub);
    addPrimOp("__mul", 2, prim_mul);
    addPrimOp("__div", 2, prim_div);
#endif
    addPrimOp("__lessThan", 2, prim_lessThan);

    // String manipulation
    addPrimOp("toString", 1, prim_toString);
#if 0    
    addPrimOp("__substring", 3, prim_substring);
    addPrimOp("__stringLength", 1, prim_stringLength);
    addPrimOp("__unsafeDiscardStringContext", 1, prim_unsafeDiscardStringContext);
    addPrimOp("__unsafeDiscardOutputDependency", 1, prim_unsafeDiscardOutputDependency);

    // Versions
    addPrimOp("__parseDrvName", 1, prim_parseDrvName);
    addPrimOp("__compareVersions", 2, prim_compareVersions);
#endif
}


}
